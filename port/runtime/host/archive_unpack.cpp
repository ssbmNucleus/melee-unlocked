// The one archive reader of the host (archive_unpack.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "archive_unpack.h"
#include "host.h"
#include "product_identity.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>
#include <vector>
// Archives are read here, not by tar.exe: Wine and Proton have no tar.exe, so every download there
// failed with "unexpected contents". stb_image's inflate, kept private to this file.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace host::archive {
namespace {
namespace fs = std::filesystem;

constexpr uint64_t kRatioFloor = 1ull << 20;            // entries this small are not ratio-checked
constexpr uint64_t kMaxCentralDirectory = 16ull << 20;  // 65535 entries with long names fit
constexpr uint64_t kMaxInflate = 1ull << 30;            // stb's inflate takes int sizes
constexpr uint64_t kMaxTarGz = 1536ull << 20;           // a .tar.gz is unpacked in memory

uint32_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t crc32_of(const uint8_t* data, size_t n) {
  static uint32_t table[256] = {};
  if (!table[1]) for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}
std::string mb(uint64_t bytes) { return std::to_string((bytes + (1u << 19)) >> 20) + " MB"; }
// A name as a message shows it: printable ASCII, cut short.
std::string shown(const std::string& name) {
  std::string s;
  for (const unsigned char c : name) s += c >= 0x20 && c < 0x7F ? (char)c : '?';
  return s.size() > 120 ? s.substr(0, 117) + "..." : s;
}
// The key two names of one file share (Windows' file names ignore case; a folder's '/' aside).
std::string name_key(std::string name) {
  while (!name.empty() && name.back() == '/') name.pop_back();
  return lower(std::move(name));
}

bool valid_utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
    if (n == 9 || (n && c < 0xC2) || i + n >= s.size() + (n ? 0 : 1)) return false;
    for (size_t k = 1; k <= n; ++k)
      if (((unsigned char)s[i + k] >> 6) != 2) return false;
    i += n + 1;
  }
  return true;
}

// A name Windows can hold as a file (checked before anything is written): no reserved device name
// (CON, NUL, COM1, ...), no part ending in a dot or a space, UTF-8 text.
bool windows_name(const std::string& name) {
  if (!valid_utf8(name) || name.find_first_of("<>|\x7f") != std::string::npos) return false;
  size_t start = 0;
  for (;;) {
    const size_t slash = name.find('/', start);
    const std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) {
      if (part.back() == '.' || part.back() == ' ') return false;
      std::string stem = lower(part.substr(0, part.find('.')));
      while (!stem.empty() && stem.back() == ' ') stem.pop_back();
      for (const char* reserved : {"con", "prn", "aux", "nul", "conin$", "conout$"})
        if (stem == reserved) return false;
      if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '0' && stem[3] <= '9')
        return false;
    }
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

bool write_file(const fs::path& target, const uint8_t* data, size_t size) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  std::ofstream o(target, std::ios::binary | std::ios::trunc);
  if (size) o.write((const char*)data, (std::streamsize)size);
  o.close();
  return (bool)o;
}

// A .tar.gz (one gzip member; plain, GNU long-name and pax-path tar entries) checked whole, then
// written. Links, devices and the like are skipped.
bool extract_tar_gz(const fs::path& archive, const fs::path& dest, const Limits& limits, std::string* error,
                    std::string* unwritable) {
  const auto fail = [&](const std::string& why) { *error = why; return false; };
  std::error_code ec;
  const uint64_t file_size = fs::file_size(archive, ec);
  if (ec) return fail("The archive could not be opened.");
  if (file_size > kMaxTarGz) return fail("The archive is larger than " + mb(kMaxTarGz) + ".");
  std::vector<uint8_t> gz((size_t)file_size);
  {
    std::ifstream f(archive, std::ios::binary);
    if (!f || (file_size && !f.read((char*)gz.data(), (std::streamsize)file_size))) return fail("The archive could not be read.");
  }
  if (gz.size() < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8) return fail("The archive is not a .tar.gz.");
  const unsigned flags = gz[3];
  size_t at = 10;
  if (flags & 4) { if (at + 2 > gz.size()) return fail("The .tar.gz is damaged."); at += 2 + rd16(&gz[at]); }
  if (flags & 8) { while (at < gz.size() && gz[at]) ++at; ++at; }
  if (flags & 16) { while (at < gz.size() && gz[at]) ++at; ++at; }
  if (flags & 2) at += 2;
  if (at + 8 > gz.size()) return fail("The .tar.gz is damaged (cut short).");
  const uint32_t crc = rd32(&gz[gz.size() - 8]), size = rd32(&gz[gz.size() - 4]);
  // The size the gzip trailer gives is the most it unpacks to (the buffer is that size).
  if (size > kMaxTarGz || size > limits.max_total_bytes + 64ull * 1024 * (limits.max_entries + 1ull))
    return fail("The .tar.gz unpacks to more than the " + mb(limits.max_total_bytes) + " limit.");
  const uint64_t packed = gz.size() - at - 8;
  if (limits.max_ratio && size > kRatioFloor && size > (uint64_t)limits.max_ratio * packed)
    return fail("The .tar.gz packs more than " + std::to_string(limits.max_ratio) + " to 1.");
  std::vector<uint8_t> tar(size);
  gz.resize(gz.size() - 8);
  gz.insert(gz.end(), 16, 0);   // stb's inflate reads a few bytes past the stream (see Zip::read)
  if (size && stbi_zlib_decode_noheader_buffer((char*)tar.data(), (int)size, (const char*)gz.data() + at, (int)(gz.size() - at)) != (int)size)
    return fail("The .tar.gz is damaged.");
  std::vector<uint8_t>().swap(gz);
  if (crc32_of(tar.data(), tar.size()) != crc) return fail("The .tar.gz is damaged (CRC).");

  // First every header, checked; nothing is written for an archive that breaks a rule.
  struct Item { std::string name; bool folder; size_t data; uint64_t length; };
  std::vector<Item> items;
  std::set<std::string> seen;
  std::string long_name;
  uint64_t headers = 0, total = 0, skipped = 0;
  for (size_t pos = 0; pos + 512 <= tar.size();) {
    const unsigned char* h = &tar[pos];
    if (std::all_of(h, h + 512, [](unsigned char c) { return c == 0; })) break;
    // Every entry has a header, and a long name or pax record one more: twice the entries at most.
    if (++headers > 2ull * limits.max_entries + 2) return fail("The .tar.gz has more than " + std::to_string(limits.max_entries) + " entries.");
    uint64_t length = 0;
    for (int i = 124; i < 136 && h[i] >= '0' && h[i] <= '7'; ++i) length = length * 8 + (h[i] - '0');
    const size_t data = pos + 512;
    if (length > tar.size() - data) return fail("The .tar.gz is damaged (cut short).");
    const char type = (char)h[156];
    std::string name;
    if (!long_name.empty()) { name.swap(long_name); }
    else {
      const std::string prefix((const char*)h + 345, strnlen((const char*)h + 345, 155));
      name.assign((const char*)h, strnlen((const char*)h, 100));
      if (std::memcmp(h + 257, "ustar", 5) == 0 && !prefix.empty()) name = prefix + "/" + name;
    }
    if (type == 'L') long_name.assign((const char*)&tar[data], strnlen((const char*)&tar[data], (size_t)length));
    else if (type == 'x') {   // pax: "<length> path=<name>\n" records
      const std::string records((const char*)&tar[data], (size_t)length);
      for (size_t r = 0; r < records.size();) {
        const size_t space = records.find(' ', r);
        const size_t len = std::strtoul(records.c_str() + r, nullptr, 10);
        if (space == std::string::npos || len == 0 || r + len > records.size() || space >= r + len) break;
        const std::string record = records.substr(space + 1, r + len - space - 2);
        if (record.rfind("path=", 0) == 0) long_name = record.substr(5);
        r += len;
      }
    } else if (type == '0' || type == '\0' || type == '5') {
      while (!name.empty() && name.back() == '/') name.pop_back();
      while (name.rfind("./", 0) == 0) name.erase(0, 2);
      if (!name.empty() && name != ".") {
        if (!safe_name(name) || !windows_name(name)) return fail("The .tar.gz has an unsafe name: " + shown(name));
        if (!seen.insert(name_key(name)).second) return fail("The .tar.gz names one file twice (letter case aside): " + shown(name));
        if (items.size() >= limits.max_entries) return fail("The .tar.gz has more than " + std::to_string(limits.max_entries) + " entries.");
        const bool folder = type == '5';
        if (!folder) {
          if (length > limits.max_entry_bytes) return fail("A file in the .tar.gz is larger than the " + mb(limits.max_entry_bytes) + " limit: " + shown(name));
          total += length;
          if (total > limits.max_total_bytes) return fail("The .tar.gz unpacks to more than the " + mb(limits.max_total_bytes) + " limit.");
        }
        items.push_back({name, folder, data, folder ? 0 : length});
      }
    } else if (type != 'g') {
      ++skipped;   // links, devices: never made (no download needs them)
    }
    pos = data + (size_t)((length + 511) / 512 * 512);
  }
  for (const Item& item : items) {
    const fs::path target = dest / fs::u8path(item.name);
    std::error_code made;
    if (item.folder) {
      fs::create_directories(target, made);
      if (made) { if (unwritable) *unwritable = target.u8string(); return fail("A folder could not be made: " + target.u8string()); }
    } else if (!write_file(target, &tar[item.data], (size_t)item.length)) {
      if (unwritable) *unwritable = target.u8string();
      return fail("A file could not be written: " + target.u8string());
    }
  }
  if (skipped) host::log("archive: %s: %llu links or special entries skipped", archive.filename().u8string().c_str(), (unsigned long long)skipped);
  return true;
}
}  // namespace

bool safe_name(const std::string& raw) {
  if (raw.empty() || raw.size() > 1024 || raw.front() == '/') return false;
  for (const unsigned char c : raw)
    if (c < 0x20 || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?') return false;
  std::string part;
  for (size_t i = 0; i <= raw.size(); ++i) {
    const char c = i == raw.size() ? '/' : raw[i];
    if (c != '/') { part += c; continue; }
    // Only a folder's own name may end in '/'; no empty, "." or ".." part.
    if (part.empty()) return i == raw.size() && i > 0 && raw[i - 1] == '/';
    if (part == "." || part == "..") return false;
    part.clear();
  }
  return true;
}

bool Zip::read_at(uint64_t offset, void* out, uint64_t size) const {
  if (in_memory_) {
    if (offset > memory_.size() || size > memory_.size() - offset) return false;
    if (size) std::memcpy(out, memory_.data() + offset, (size_t)size);
    return true;
  }
  if (!stream_.is_open()) stream_.open(file_, std::ios::binary);
  if (!stream_) return false;
  stream_.clear();
  stream_.seekg((std::streamoff)offset);
  return size == 0 || (bool)stream_.read((char*)out, (std::streamsize)size);
}

bool Zip::open(const fs::path& file, const Limits& limits, std::string* error) {
  entries_.clear();
  memory_.clear();
  in_memory_ = false;
  if (stream_.is_open()) stream_.close();
  file_ = file;
  std::error_code ec;
  const uint64_t total = fs::file_size(file, ec);
  if (ec) { *error = "The archive could not be opened."; return false; }
  return parse(total, limits, error);
}

bool Zip::open_memory(std::vector<uint8_t> bytes, const Limits& limits, std::string* error) {
  entries_.clear();
  if (stream_.is_open()) stream_.close();
  file_.clear();
  memory_ = std::move(bytes);
  in_memory_ = true;
  return parse(memory_.size(), limits, error);
}

bool Zip::parse(uint64_t total, const Limits& limits, std::string* error) {
  const auto fail = [&](const std::string& why) { entries_.clear(); *error = why; return false; };
  if (total < 22) return fail("The archive is not a zip (it has no end record).");
  const uint64_t tail_size = std::min<uint64_t>(total, 22 + 65535);
  std::vector<uint8_t> tail((size_t)tail_size);
  if (!read_at(total - tail_size, tail.data(), tail_size)) return fail("The zip could not be read.");
  size_t eocd = std::string::npos;
  for (size_t i = (size_t)tail_size - 22 + 1; i-- > 0;)
    if (rd32(&tail[i]) == 0x06054b50u) { eocd = i; break; }
  if (eocd == std::string::npos) return fail("The zip is damaged or cut short (its end record is missing).");
  const uint8_t* end = &tail[eocd];
  const uint32_t disk = rd16(end + 4), dir_disk = rd16(end + 6), disk_entries = rd16(end + 8), count = rd16(end + 10);
  const uint32_t dir_size = rd32(end + 12), dir_at = rd32(end + 16), comment = rd16(end + 20);
  if (count == 0xFFFF || dir_size == 0xFFFFFFFFu || dir_at == 0xFFFFFFFFu) return fail("Zip64 archives are not supported.");
  if (disk || dir_disk || disk_entries != count) return fail("Zips split over several disks are not supported.");
  const uint64_t end_at = total - tail_size + eocd;
  if (eocd + 22 + comment > tail_size || (uint64_t)dir_at + dir_size > end_at)
    return fail("The zip is damaged (its central directory points outside it).");
  if (count > limits.max_entries) return fail("The zip has more than " + std::to_string(limits.max_entries) + " entries.");
  if (dir_size > kMaxCentralDirectory) return fail("The zip's central directory is larger than " + mb(kMaxCentralDirectory) + ".");
  std::vector<uint8_t> d(dir_size);
  if (dir_size && !read_at(dir_at, d.data(), dir_size)) return fail("The zip could not be read.");
  std::set<std::string> seen;
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  uint64_t unpacked = 0;
  size_t at = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 46 > d.size() || rd32(&d[at]) != 0x02014b50u) return fail("The zip's central directory is damaged.");
    const uint8_t* h = &d[at];
    Entry e;
    e.flags = (uint16_t)rd16(h + 8);
    e.method = (uint16_t)rd16(h + 10);
    e.crc = rd32(h + 16);
    e.packed = rd32(h + 20);
    e.size = rd32(h + 24);
    const uint32_t name_len = rd16(h + 28), extra = rd16(h + 30), comment_len = rd16(h + 32), start_disk = rd16(h + 34);
    const uint32_t attributes = rd32(h + 38), local = rd32(h + 42);
    if (!name_len || at + 46 + name_len + extra + comment_len > d.size()) return fail("The zip's central directory is damaged.");
    e.name.assign((const char*)h + 46, name_len);
    at += 46 + name_len + extra + comment_len;
    if (e.packed == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu || local == 0xFFFFFFFFu) return fail("Zip64 archives are not supported.");
    if (!safe_name(e.name)) return fail("The zip has an unsafe name: " + shown(e.name));
    if (!seen.insert(name_key(e.name)).second) return fail("The zip names one file twice (letter case aside): " + shown(e.name));
    if (e.flags & 0x41) return fail("Encrypted zip entries are not supported.");
    if (e.method != 0 && e.method != 8) return fail("A zip entry is packed in a way this game does not read: " + shown(e.name));
    if (((attributes >> 16) & 0170000) == 0120000) return fail("Symbolic links are not allowed in archives: " + shown(e.name));
    if (start_disk != 0 || local >= dir_at) return fail("The zip is damaged (an entry points outside it).");
    if (e.method == 0 && e.packed != e.size) return fail("The zip is damaged (a stored entry's sizes differ).");
    if (e.size > limits.max_entry_bytes)
      return fail("A file in the zip is larger than the " + mb(limits.max_entry_bytes) + " limit: " + shown(e.name));
    unpacked += e.size;
    if (unpacked > limits.max_total_bytes) return fail("The zip unpacks to more than the " + mb(limits.max_total_bytes) + " limit.");
    if (limits.max_ratio && e.size > kRatioFloor && e.size > (uint64_t)limits.max_ratio * e.packed)
      return fail("A file in the zip packs more than " + std::to_string(limits.max_ratio) + " to 1: " + shown(e.name));
    // The local header must be the same entry: a central name cannot vouch for other data.
    uint8_t lh[30];
    if (!read_at(local, lh, 30) || rd32(lh) != 0x04034b50u) return fail("The zip is damaged (a local header is missing).");
    const uint32_t local_name = rd16(lh + 26), local_extra = rd16(lh + 28);
    std::string name(local_name, '\0');
    if (local_name != name_len || !read_at(local + 30, name.data(), local_name) || name != e.name || rd16(lh + 8) != e.method ||
        (rd16(lh + 6) & 0x41))
      return fail("The zip is damaged (a local header differs from its directory entry).");
    e.data = (uint64_t)local + 30 + local_name + local_extra;
    if (e.data > dir_at || e.packed > dir_at - e.data) return fail("The zip is damaged or cut short (an entry's data runs past it).");
    spans.emplace_back(local, e.data + e.packed);
    entries_.push_back(std::move(e));
  }
  if (at != d.size()) return fail("The zip's central directory is damaged (records past its entries).");
  // Each entry's local header and data in a span no other entry shares: a zip whose central directory
  // named one compressed block many times unpacked a 17 KB file to 335 MB.
  std::sort(spans.begin(), spans.end());
  for (size_t i = 1; i < spans.size(); ++i)
    if (spans[i].first < spans[i - 1].second) return fail("The zip is damaged (entries share their data).");
  return true;
}

std::string Entry::extension() const {
  const size_t slash = name.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot <= start) return {};   // none, or a name starting with its dot
  return lower(name.substr(dot));
}

const Entry* Zip::find(const std::string& name) const {
  for (const Entry& e : entries_)
    if (e.name == name) return &e;
  return nullptr;
}

bool Zip::read(const Entry& e, std::vector<uint8_t>* out, std::string* error) const {
  out->clear();
  if (e.folder()) return true;
  if (e.size > kMaxInflate || e.packed > kMaxInflate) { *error = "A file in the zip is too large to read: " + shown(e.name); return false; }
  // stb's inflate looks a few bytes past the end of the stream; without padding it calls a stream
  // that ends exactly there (zlib's own output) a bad huffman code.
  std::vector<uint8_t> packed((size_t)e.packed + (e.method == 8 ? 16 : 0), 0);
  if (e.packed && !read_at(e.data, packed.data(), e.packed)) { *error = "The zip could not be read: " + shown(e.name); return false; }
  if (e.method == 0) {
    packed.swap(*out);
  } else {
    out->resize((size_t)e.size);
    if (e.size && stbi_zlib_decode_noheader_buffer((char*)out->data(), (int)e.size, (const char*)packed.data(), (int)packed.size()) != (int)e.size) {
      out->clear();
      *error = "The zip is damaged: " + shown(e.name);
      return false;
    }
  }
  if (crc32_of(out->data(), out->size()) != e.crc) {
    out->clear();
    *error = "The zip is damaged (CRC): " + shown(e.name);
    return false;
  }
  return true;
}

bool Zip::extract(const fs::path& dest, std::string* error, std::string* unwritable) const {
  if (unwritable) unwritable->clear();
  for (const Entry& e : entries_)
    if (!windows_name(e.name)) { *error = "The zip has a name Windows cannot hold as a file: " + shown(e.name); return false; }
  std::error_code ec;
  fs::create_directories(dest, ec);
  std::vector<uint8_t> data;
  for (const Entry& e : entries_) {
    const fs::path target = dest / fs::u8path(e.name);
    if (e.folder()) {
      fs::create_directories(target, ec);
      if (ec) { if (unwritable) *unwritable = target.u8string(); *error = "A folder could not be made: " + target.u8string(); return false; }
      continue;
    }
    if (!read(e, &data, error)) return false;
    if (!write_file(target, data.data(), data.size())) {
      if (unwritable) *unwritable = target.u8string();
      *error = "A file could not be written: " + target.u8string();
      return false;
    }
  }
  return true;
}

bool extract(const fs::path& archive, const fs::path& dest, const Limits& limits, std::string* error, std::string* unwritable) {
  if (unwritable) unwritable->clear();
  unsigned char magic[2] = {};
  {
    std::ifstream f(archive, std::ios::binary);
    if (!f || !f.read((char*)magic, 2)) { *error = "The archive could not be opened."; return false; }
  }
  if (magic[0] == 0x1f && magic[1] == 0x8b) return extract_tar_gz(archive, dest, limits, error, unwritable);
  if (magic[0] != 'P' || magic[1] != 'K') { *error = "The archive is not a zip or a .tar.gz."; return false; }
  Zip zip;
  return zip.open(archive, limits, error) && zip.extract(dest, error, unwritable);
}

// The plain reason for a failed write beside the game, with what to do about the usual one.
std::string place_error(const std::string& what, const std::filesystem::path& dir, const std::error_code& ec) {
  std::string m = what + " in " + dir.u8string() + " (" + (ec ? ec.message() : std::string("Windows refused the write")) + ").";
  if (!ec || ec.value() == ERROR_ACCESS_DENIED || ec == std::errc::permission_denied || ec.value() == ERROR_SHARING_VIOLATION ||
      ec.value() == ERROR_LOCK_VIOLATION)
    m += " Windows or an antivirus did not let the game write in its folder: move the " PRODUCT_NAME " folder out of "
         "Program Files, Desktop, Documents and Downloads (for example to C:\\Games), or allow the game under "
         "Windows Security > Ransomware protection, then try again.";
  return m;
}

bool place_file(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ec) { std::error_code ignore; fs::remove(from, ignore); return true; }
    Sleep(250);
  }
  return false;
}

bool place_folder(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
  ec.clear();
  fs::rename(from, to, ec);
  if (!ec) return true;
  for (const auto& e : fs::recursive_directory_iterator(from, ec)) {
    const fs::path target = to / fs::relative(e.path(), from);
    if (e.is_directory()) { fs::create_directories(target, ec); if (ec) return false; continue; }
    fs::create_directories(target.parent_path(), ec);
    if (!place_file(e.path(), target, ec)) return false;
  }
  return !ec;
}
}  // namespace host::archive
