// Fighter plugin approval: see plugin_gate.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_gate.h"
#include "../runtime/host/product_identity.h"   // the name on the approval box

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "monocypher-ed25519.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace plugin_gate {

namespace {
// The game library reads nothing larger (mexft_plugins.c read_file); nor does this.
constexpr uint64_t kMaxPluginBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxStoreBytes = 4ull * 1024 * 1024;
constexpr char kSignaturePrefix[] = "MUFTSIG1";
constexpr char kExtension[] = ".mufighter";

std::string lower(std::string text) {
  for (char& c : text) c = (char)std::tolower((unsigned char)c);
  return text;
}

// ---- SHA-256 (FIPS 180-4) ----
constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int t = 0; t < 16; ++t)
    w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
  for (int t = 16; t < 64; ++t) {
    const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
    const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int t = 0; t < 64; ++t) {
    const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[t] + w[t];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool zero_key(const Key& key) {
  for (uint8_t b : key)
    if (b) return false;
  return true;
}

// ---- reading files ----
// `share`: what others may do while this reads (Windows share flags; ignored elsewhere).
bool read_file(const fs::path& path, uint64_t limit, std::vector<uint8_t>* out, std::string* error, bool exclusive_writes) {
  out->clear();
#ifdef _WIN32
  const DWORD share = exclusive_writes ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    *error = code == ERROR_SHARING_VIOLATION ? "the file is open for writing in another program"
                                             : "the file cannot be opened (system error " + std::to_string(code) + ")";
    return false;
  }
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) != 0;
  if (!ok) *error = "its size cannot be read";
  else if ((uint64_t)size.QuadPart > limit) { *error = "the file is larger than " + std::to_string(limit >> 20) + " MB"; ok = false; }
  if (ok) {
    out->resize((size_t)size.QuadPart);
    size_t at = 0;
    while (ok && at < out->size()) {
      DWORD got = 0;
      const DWORD want = (DWORD)std::min<size_t>(out->size() - at, 1u << 24);
      if (!ReadFile(file, out->data() + at, want, &got, nullptr) || got == 0) ok = false;
      at += got;
    }
    if (!ok) *error = "the file cannot be read";
  }
  CloseHandle(file);
  if (!ok) out->clear();
  return ok;
#else
  (void)exclusive_writes;
  std::ifstream in(path, std::ios::binary);
  if (!in) { *error = "the file cannot be opened"; return false; }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0 || (uint64_t)size > limit) { *error = "the file is too large"; return false; }
  in.seekg(0);
  out->resize((size_t)size);
  if (size && !in.read((char*)out->data(), size)) { out->clear(); *error = "the file cannot be read"; return false; }
  return true;
#endif
}

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

// The plugin object, read without trusting a single field of it.
struct Coff {
  const std::vector<uint8_t>& b;
  uint32_t sections = 0, symtab = 0, symbols = 0, strings = 0, strings_size = 0;
  bool ok = false;
  explicit Coff(const std::vector<uint8_t>& bytes) : b(bytes) {
    if (b.size() < 20 || le16(b.data()) != 0x8664 || le16(b.data() + 16) != 0) return;
    sections = le16(b.data() + 2);
    symtab = le32(b.data() + 8);
    symbols = le32(b.data() + 12);
    if ((uint64_t)20 + (uint64_t)sections * 40 > b.size()) return;
    if ((uint64_t)symtab + (uint64_t)symbols * 18 + 4 > b.size()) return;
    strings = symtab + symbols * 18;
    strings_size = le32(b.data() + strings);
    if ((uint64_t)strings + strings_size > b.size()) return;
    ok = true;
  }
  const uint8_t* section(int number) const {   // 1-based
    return number >= 1 && (uint32_t)number <= sections ? b.data() + 20 + (size_t)(number - 1) * 40 : nullptr;
  }
  // `size` bytes of section `number`'s raw data at `offset`.
  const uint8_t* at(int number, uint64_t offset, uint64_t size) const {
    const uint8_t* s = section(number);
    if (!s) return nullptr;
    const uint32_t raw = le32(s + 16), pointer = le32(s + 20);
    if (pointer == 0 || (uint64_t)pointer + raw > b.size() || offset + size > raw) return nullptr;
    return b.data() + pointer + offset;
  }
  std::string symbol_name(uint32_t index) const {
    const uint8_t* s = b.data() + symtab + (size_t)index * 18;
    if (le32(s) == 0) {
      const uint32_t at = le32(s + 4);
      if (at >= strings_size) return {};
      const char* start = (const char*)b.data() + strings + at;
      const size_t room = strings_size - at;
      return std::string(start, strnlen(start, room));
    }
    return std::string((const char*)s, strnlen((const char*)s, 8));
  }
  // Where the 8-byte pointer at `offset` of section `number` points: the section and offset its
  // ADDR64 relocation and stored addend give. False when it has none (a null pointer).
  bool pointer(int number, uint64_t offset, int* target, uint64_t* target_offset) const {
    const uint8_t* s = section(number);
    const uint8_t* field = at(number, offset, 8);
    if (!s || !field) return false;
    const uint32_t relocs = le32(s + 24), count = le16(s + 32);
    if ((uint64_t)relocs + (uint64_t)count * 10 > b.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* r = b.data() + relocs + (size_t)i * 10;
      if (le32(r) != offset || le16(r + 8) != 1 /* IMAGE_REL_AMD64_ADDR64 */) continue;
      const uint32_t index = le32(r + 4);
      if (index >= symbols) return false;
      const uint8_t* sym = b.data() + symtab + (size_t)index * 18;
      const int16_t in = (int16_t)le16(sym + 12);
      if (in <= 0) return false;
      *target = in;
      *target_offset = (uint64_t)le32(sym + 8) + le64(field);
      return *target_offset < (1ull << 32);
    }
    return false;
  }
  std::string text(int number, uint64_t offset) const {
    std::string out;
    for (uint64_t i = 0; i < 64; ++i) {
      const uint8_t* c = at(number, offset + i, 1);
      if (!c) return {};
      if (*c == 0) return out;
      if (*c < 0x20 || *c > 0x7E) return {};
      out += (char)*c;
    }
    return {};
  }
};

fs::path sig_path(const fs::path& plugin) {
  fs::path p = plugin;
  p += ".sig";
  return p;
}

std::string json_text(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

#ifdef _WIN32
std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  std::wstring out((size_t)std::max(n, 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
  return out;
}

// The game library takes its folder in the process code page and at most 200 bytes (source_host_mods_menu.cpp).
bool fits_game_library(const fs::path& folder) {
  const std::wstring wide = folder.wstring();
  BOOL lossy = FALSE;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr,
                                        GetACP() == CP_UTF8 ? nullptr : &lossy);
  return bytes > 0 && bytes <= 200 && !lossy;
}

bool process_alive(unsigned long pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (p) {
    DWORD code = 0;
    const bool running = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return running;
  }
  return GetLastError() != ERROR_INVALID_PARAMETER;   // access denied: it exists
}
#endif

unsigned long own_pid() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return (unsigned long)getpid();
#endif
}

// Copy folders left by runs that have ended (a run ends without cleaning up after itself).
void clean_stale(const fs::path& root) {
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if (name.empty() || name.size() > 10 || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    const unsigned long pid = std::strtoul(name.c_str(), nullptr, 10);
    if (pid == own_pid()) continue;
#ifdef _WIN32
    if (process_alive(pid)) continue;
#endif
    std::error_code ignored;
    fs::remove_all(it->path(), ignored);
  }
}

fs::path data_dir(const std::string& settings_path) {
  std::error_code ec;
  fs::path parent = fs::u8path(settings_path.empty() ? std::string("port-settings.ini") : settings_path).parent_path();
  if (parent.empty()) parent = fs::current_path(ec);
  const fs::path absolute = fs::absolute(parent, ec);
  return ec ? parent : absolute;
}

std::string approval_name(int approval) { return approval > 0 ? "approved" : approval < 0 ? "refused" : "none"; }

json plugin_json(const Plugin& p) {
  json j;
  j["file"] = p.file;
  j["path"] = p.path.u8string();
  if (!p.problem.empty()) {
    j["error"] = p.problem;
    j["status"] = "blocked";
    return j;
  }
  j["name"] = p.name;
  j["size"] = p.size;
  j["sha256"] = p.sha256;
  j["signature"] = signature_name(p.signature);
  j["approval"] = approval_name(p.approval);
  j["approval_date"] = p.approval_date.empty() ? json() : json(p.approval_date);
  j["status"] = status_name(p.status);
  return j;
}
}  // namespace

// ---- hashing ----
void sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= size; i += 64) sha256_block(h, data + i);
  uint8_t tail[128] = {};
  const size_t rest = size - i;
  if (rest) std::memcpy(tail, data + i, rest);
  tail[rest] = 0x80;
  const size_t total = rest + 1 + 8 <= 64 ? 64 : 128;
  const uint64_t bits = (uint64_t)size * 8;
  for (int k = 0; k < 8; ++k) tail[total - 1 - k] = (uint8_t)(bits >> (8 * k));
  sha256_block(h, tail);
  if (total == 128) sha256_block(h, tail + 64);
  for (int k = 0; k < 8; ++k) {
    out[4 * k] = (uint8_t)(h[k] >> 24);
    out[4 * k + 1] = (uint8_t)(h[k] >> 16);
    out[4 * k + 2] = (uint8_t)(h[k] >> 8);
    out[4 * k + 3] = (uint8_t)h[k];
  }
}

std::string hex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 15];
  }
  return out;
}

std::string sha256_hex(const uint8_t* data, size_t size) {
  uint8_t digest[32];
  sha256(data, size, digest);
  return hex(digest, 32);
}

bool is_sha256_hex(const std::string& text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return hex_value(c) >= 0; });
}

// ---- signatures ----
const char* signature_name(Signature s) {
  return s == Signature::Valid ? "valid" : s == Signature::Invalid ? "invalid" : "none";
}

std::vector<uint8_t> signed_message(const uint8_t digest[32]) {
  std::vector<uint8_t> message(kSignaturePrefix, kSignaturePrefix + 8);
  message.insert(message.end(), digest, digest + 32);
  return message;
}

Signature check_signature(const uint8_t digest[32], const std::vector<uint8_t>& sig_file, const std::vector<Key>& keys) {
  if (sig_file.empty()) return Signature::None;
  uint8_t signature[64];
  if (sig_file.size() == 64) {
    std::memcpy(signature, sig_file.data(), 64);
  } else {
    size_t a = 0, b = sig_file.size();
    while (a < b && std::isspace(sig_file[a])) ++a;
    while (b > a && std::isspace(sig_file[b - 1])) --b;
    if (b - a != 128) return Signature::Invalid;
    for (size_t i = 0; i < 64; ++i) {
      const int hi = hex_value((char)sig_file[a + 2 * i]), lo = hex_value((char)sig_file[a + 2 * i + 1]);
      if (hi < 0 || lo < 0) return Signature::Invalid;
      signature[i] = (uint8_t)(hi << 4 | lo);
    }
  }
  const std::vector<uint8_t> message = signed_message(digest);
  for (const Key& key : keys) {
    if (zero_key(key)) continue;   // an empty slot verifies nothing
    if (crypto_ed25519_check(signature, key.data(), message.data(), message.size()) == 0) return Signature::Valid;
  }
  return Signature::Invalid;
}

// ---- the approval store ----
fs::path store_path(const std::string& settings_path) { return data_dir(settings_path) / "plugin-approvals.json"; }

Store load_store(const fs::path& path) {
  Store store;
  std::error_code ec;
  if (!fs::exists(path, ec)) return store;
  std::vector<uint8_t> bytes;
  std::string error;
  if (!read_file(path, kMaxStoreBytes, &bytes, &error, false)) {
    store.error = error;
    return store;
  }
  const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("plugins") || !j["plugins"].is_object()) {
    store.error = "it is not a plugin approvals file";
    return store;
  }
  for (const auto& item : j["plugins"].items()) {
    const std::string key = lower(item.key());
    const json& v = item.value();
    if (!is_sha256_hex(key) || !v.is_object() || !v.contains("approved") || !v["approved"].is_boolean()) continue;
    Approval a;
    a.approved = v["approved"].get<bool>();
    if (v.contains("name") && v["name"].is_string()) a.name = v["name"].get<std::string>();
    if (v.contains("file") && v["file"].is_string()) a.file = v["file"].get<std::string>();
    if (v.contains("date") && v["date"].is_string()) a.date = v["date"].get<std::string>();
    store.entries[key] = a;
  }
  return store;
}

std::string now_utc() {
  const std::time_t t = std::time(nullptr);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

bool update_store(const fs::path& path, const std::function<void(Store&)>& change, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path lock_path = path;
  lock_path += ".lock";
#ifdef _WIN32
  HANDLE lock = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 100 && lock == INVALID_HANDLE_VALUE; ++attempt) {
    lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) break;
      Sleep(50);
    }
  }
  if (lock == INVALID_HANDLE_VALUE) {
    *error = "the approvals file is locked by another program (" + lock_path.u8string() + ")";
    return false;
  }
#endif
  bool ok = false;
  Store store = load_store(path);
  if (!store.error.empty()) {
    // Unreadable: kept aside, not overwritten in place, and a new one started.
    fs::path aside = path;
    aside += ".unreadable";
    fs::rename(path, aside, ec);
    store = Store{};
  }
  change(store);
  json plugins = json::object();
  for (const auto& [sha, a] : store.entries)
    plugins[sha] = {{"approved", a.approved}, {"name", a.name}, {"file", a.file}, {"date", a.date}};
  const json doc = {{"format", 1},
                    {"about", "Fighter plugins this player allowed (approved true) or refused (false), by the SHA-256 of "
                              "the plugin file. melee_source --approve-plugin <file> and --revoke-plugin <sha256> change it."},
                    {"plugins", plugins}};
  const std::string text = doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
  fs::path part = path;
  part += ".part";
#ifdef _WIN32
  HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ok = WriteFile(out, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size() && FlushFileBuffers(out);
    CloseHandle(out);
  }
  for (int attempt = 0; ok && attempt < 20; ++attempt) {
    if (MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
    if (attempt == 19) ok = false;
    Sleep(50);
  }
  if (!ok) {
    DeleteFileW(part.c_str());
    *error = "the approvals file cannot be written (" + path.u8string() + ")";
  }
  CloseHandle(lock);
#else
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out << text;
    ok = (bool)out;
  }
  if (ok) fs::rename(part, path, ec);
  ok = ok && !ec;
  if (!ok) *error = "the approvals file cannot be written (" + path.u8string() + ")";
#endif
  return ok;
}

// ---- one plugin file ----
const char* status_name(Status s) { return s == Status::Signed ? "signed" : s == Status::Approved ? "approved" : "blocked"; }

bool read_whole(const fs::path& path, std::vector<uint8_t>* out, std::string* error) {
  return read_file(path, kMaxPluginBytes, out, error, true);
}

bool looks_like_plugin(const std::vector<uint8_t>& bytes) {
  static const char record[] = "mu_fighter_plugin";
  return bytes.size() >= 64 && bytes[0] == 0x64 && bytes[1] == 0x86 &&
         std::search(bytes.begin(), bytes.end(), record, record + sizeof record - 1) != bytes.end();
}

std::string fighter_name(const std::vector<uint8_t>& bytes) {
  const Coff o(bytes);
  if (!o.ok) return {};
  for (uint32_t i = 0; i < o.symbols; i += 1 + o.b[o.symtab + (size_t)i * 18 + 17]) {
    const uint8_t* sym = o.b.data() + o.symtab + (size_t)i * 18;
    const int16_t section = (int16_t)le16(sym + 12);
    if (section <= 0 || sym[16] != 2 /* IMAGE_SYM_CLASS_EXTERNAL */ || o.symbol_name(i) != "mu_fighter_plugin") continue;
    // MuFighterPlugin (mexft_plugin.h): magic, version, abi[16], then the fighters pointer at 24.
    const uint64_t record = le32(sym + 8);
    const uint8_t* magic = o.at(section, record, 4);
    if (!magic || le32(magic) != 0x5446554Du) return {};
    int list = 0, name_section = 0, descriptor = 0;
    uint64_t list_at = 0, name_at = 0, descriptor_at = 0;
    if (!o.pointer(section, record + 24, &list, &list_at)) return {};
    // MuFighterPluginEntry: descriptor, then mex_name. The m-ex name first, else the descriptor's.
    if (o.pointer(list, list_at + 8, &name_section, &name_at)) {
      const std::string name = o.text(name_section, name_at);
      if (!name.empty()) return name;
    }
    if (o.pointer(list, list_at, &descriptor, &descriptor_at) && o.pointer(descriptor, descriptor_at, &name_section, &name_at))
      return o.text(name_section, name_at);
    return {};
  }
  return {};
}

Plugin examine(const fs::path& file, const Store& store, const std::vector<Key>& keys) {
  Plugin p;
  p.path = file;
  p.file = file.filename().u8string();
  p.name = file.stem().u8string();
  if (!read_whole(file, &p.bytes, &p.problem)) return p;
  p.size = p.bytes.size();
  uint8_t digest[32];
  sha256(p.bytes.data(), p.bytes.size(), digest);
  p.sha256 = hex(digest, 32);
  const std::string name = fighter_name(p.bytes);
  if (!name.empty()) p.name = name;
  std::error_code ec;
  const fs::path sig = sig_path(file);
  if (fs::is_regular_file(sig, ec)) {
    std::vector<uint8_t> sig_bytes;
    std::string sig_error;
    p.signature = read_file(sig, 4096, &sig_bytes, &sig_error, false) ? check_signature(digest, sig_bytes, keys)
                                                                      : Signature::Invalid;
    if (p.signature == Signature::None) p.signature = Signature::Invalid;   // an empty .sig is no signature that verifies
  }
  const auto entry = store.entries.find(p.sha256);
  if (entry != store.entries.end()) {
    p.approval = entry->second.approved ? 1 : -1;
    p.approval_date = entry->second.date;
  }
  p.status = p.signature == Signature::Valid ? Status::Signed : p.approval > 0 ? Status::Approved : Status::Blocked;
  return p;
}

std::vector<Plugin> scan(const fs::path& folder, const Store& store, const std::vector<Key>& keys) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code type_error;
    if (!it->is_regular_file(type_error)) continue;
    if (lower(it->path().extension().u8string()) == kExtension) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    return lower(a.filename().u8string()) < lower(b.filename().u8string());
  });
  std::vector<Plugin> out;
  for (const auto& f : files) out.push_back(examine(f, store, keys));
  return out;
}

fs::path default_plugin_folder(const fs::path& library_dir) {
#ifdef _WIN32
  wchar_t named[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"MELEE_FIGHTER_PLUGINS", named, MAX_PATH);
  if (n > 0 && n < MAX_PATH - 16) return fs::path(named);
#else
  const char* named = std::getenv("MELEE_FIGHTER_PLUGINS");
  if (named && *named) return fs::u8path(named);
#endif
  return library_dir / "fighters";
}

// ---- the gate ----
Gate::Gate(fs::path store, fs::path stage, std::vector<Key> keys, Log log)
    : store_(std::move(store)), stage_(std::move(stage)), keys_(std::move(keys)), log_(log) {}

Gate::~Gate() { release(); }

void Gate::say(const std::string& key, const std::string& line) {
  if (said_.count(key)) return;
  said_[key] = line;
  if (log_) log_(line.c_str());
}

std::map<std::string, std::string> Gate::staged() const {
  std::map<std::string, std::string> out;
  for (const auto& [name, held] : held_) out[name] = held.sha256;
  return out;
}

void Gate::release() {
#ifdef _WIN32
  for (auto& [name, held] : held_)
    if (held.handle) CloseHandle((HANDLE)held.handle);
#endif
  held_.clear();
  std::error_code ec;
  if (!stage_.empty()) fs::remove_all(stage_, ec);
}

bool Gate::place(const Plugin& p) {
  const std::string key = lower(p.file);
  const auto it = held_.find(key);
  if (it != held_.end() && it->second.sha256 == p.sha256) return true;
  const fs::path target = stage_ / fs::u8path(p.file);
  fs::path part = target;
  part += ".part";
  std::string problem;
  std::error_code made;
  fs::create_directories(stage_, made);
  if (!fs::is_directory(stage_, made)) {
    say(p.file + "|" + p.sha256 + "|place", "plugins: " + p.file + ": not loaded, the folder for this run's plugins (" +
                                                stage_.u8string() + ") cannot be made; its fighter is unavailable this run");
    return false;
  }
#ifdef _WIN32
  if (it != held_.end()) {
    CloseHandle((HANDLE)it->second.handle);
    held_.erase(it);
  }
  HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool ok = out != INVALID_HANDLE_VALUE;
  if (ok) {
    DWORD written = 0;
    ok = p.bytes.empty() || (WriteFile(out, p.bytes.data(), (DWORD)p.bytes.size(), &written, nullptr) && written == p.bytes.size());
    CloseHandle(out);
  }
  ok = ok && MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!ok) {
    problem = "it cannot be copied into " + stage_.u8string() + " (system error " + std::to_string(GetLastError()) + ")";
    DeleteFileW(part.c_str());
  }
  // Opened again with writes and deletes shut out, then compared with the bytes that were checked:
  // from here until the run ends the game library can read only this file.
  HANDLE held = INVALID_HANDLE_VALUE;
  if (ok) {
    held = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::vector<uint8_t> back;
    LARGE_INTEGER size{};
    ok = held != INVALID_HANDLE_VALUE && GetFileSizeEx(held, &size) && (uint64_t)size.QuadPart == p.bytes.size();
    if (ok) {
      back.resize(p.bytes.size());
      size_t at = 0;
      while (ok && at < back.size()) {
        DWORD got = 0;
        ok = ReadFile(held, back.data() + at, (DWORD)std::min<size_t>(back.size() - at, 1u << 24), &got, nullptr) && got > 0;
        at += got;
      }
      ok = ok && back == p.bytes;
    }
    if (!ok) {
      problem = "its copy changed while it was being prepared";
      if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
      DeleteFileW(target.c_str());
    }
  }
  if (ok) held_[key] = {p.sha256, held};
#else
  std::ofstream out(part, std::ios::binary | std::ios::trunc);
  out.write((const char*)p.bytes.data(), (std::streamsize)p.bytes.size());
  out.close();
  std::error_code ec;
  fs::rename(part, target, ec);
  const bool ok = (bool)out && !ec;
  if (!ok) problem = "it cannot be copied into " + stage_.u8string();
  else held_[key] = {p.sha256, nullptr};
#endif
  if (!ok)
    say(p.file + "|" + p.sha256 + "|place", "plugins: " + p.file + ": not loaded, " + problem + "; its fighter is unavailable this run");
  return ok;
}

fs::path Gate::prepare(const fs::path& source, const Prompt* ask) {
  // The folder itself is made when the first plugin goes in: until then the game library finds no
  // folder there, which to it is no plugins.
  const Store store = load_store(store_);
  if (!store.error.empty())
    say("store|" + store.error, "plugins: " + store_.u8string() + " cannot be read (" + store.error + "); no plugin counts as approved");
  for (Plugin& p : scan(source, store, keys_)) {
    if (!p.problem.empty()) {
      say(p.file + "|problem|" + p.problem, "plugins: " + p.file + ": not loaded, " + p.problem + "; its fighter is unavailable this run");
      continue;
    }
    const std::string id = p.file + " (" + p.name + ", sha256 " + p.sha256.substr(0, 16) + ")";
    const std::string seen = p.file + "|" + p.sha256 + "|";
    if (p.signature == Signature::Invalid)
      say(seen + "signature", "plugins: " + id + ": its signature does not verify" +
                                  (keys_.empty() ? std::string(" (this build trusts no signing key yet)") : std::string()) +
                                  "; it needs the player's approval like an unsigned plugin");
    if (p.status == Status::Blocked && ask && *ask && p.approval == 0 && !declined_.count(p.sha256)) {
      const bool yes = (*ask)(p);
      std::string error;
      const bool saved = update_store(store_, [&](Store& s) {
        Approval& a = s.entries[p.sha256];
        a.approved = yes;
        a.name = p.name;
        a.file = p.file;
        a.date = now_utc();
      }, &error);
      say(seen + "asked", "plugins: " + id + ": the player " + (yes ? "allowed" : "refused") + " it" +
                              (saved ? std::string() : "; the answer holds for this run only (" + error + ")"));
      if (yes) {
        p.approval = 1;
        p.status = Status::Approved;
      } else {
        p.approval = -1;
        declined_[p.sha256] = true;
      }
    }
    if (p.status == Status::Blocked) {
      say(seen + "blocked", "plugins: " + id + ": blocked, " +
                                (p.approval < 0 ? std::string("refused by the player") : std::string("not approved")) +
                                "; its fighter is unavailable this run (melee_source --approve-plugin \"" + p.path.u8string() +
                                "\" approves this file)");
      continue;
    }
    if (place(p))
      say(seen + "loads", "plugins: " + id + ": " +
                              (p.status == Status::Signed ? std::string("signed, handed to the game") : std::string("approved, handed to the game")));
  }
  return stage_;
}

// ---- this run ----
namespace {
struct Run {
  std::mutex mutex;
  std::string settings_path = "port-settings.ini";
  Log log = nullptr;
  std::unique_ptr<Gate> gate;
};
Run& run() {
  static Run r;
  return r;
}
}  // namespace

void configure_run(const std::string& settings_path, Log log) {
  Run& r = run();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.settings_path = settings_path.empty() ? std::string("port-settings.ini") : settings_path;
  r.log = log;
}

fs::path run_folder(const fs::path& source, const Prompt* ask) {
  Run& r = run();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.gate) {
    fs::path root = data_dir(r.settings_path) / "plugins-run";
    fs::path stage = root / std::to_string(own_pid());
#ifdef _WIN32
    if (!fits_game_library(stage)) {
      std::error_code ec;
      root = fs::temp_directory_path(ec) / "MeleeUnlocked-plugins-run";
      stage = root / std::to_string(own_pid());
    }
#endif
    clean_stale(root);
    r.gate = std::make_unique<Gate>(store_path(r.settings_path), stage, builtin_keys(), r.log);
  }
  return r.gate->prepare(source, ask);
}

void end_run() {
  Run& r = run();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (r.gate) r.gate->release();
  r.gate.reset();
}

bool would_load(const std::string& path) {
  std::string settings;
  {
    Run& r = run();
    std::lock_guard<std::mutex> lock(r.mutex);
    settings = r.settings_path;
  }
  const Plugin p = examine(fs::u8path(path), load_store(store_path(settings)), builtin_keys());
  return p.problem.empty() && p.status != Status::Blocked;
}

bool ask_player(const Plugin& p) {
#ifdef _WIN32
  std::string text = "A fighter plugin is waiting for your approval.\n\n";
  text += "Fighter: " + p.name + "\n";
  text += "File: " + p.path.u8string() + "\n";
  text += "SHA-256: " + p.sha256.substr(0, 16) + "...\n\n";
  text += "A fighter plugin is program code. It runs on this PC with the same rights as the game. ";
  text += p.signature == Signature::Invalid ? "Its signature does not verify. " : "It is not signed by SSBM Nucleus. ";
  text += "Allow it only if you trust where it came from.\n\n";
  text += "Load this plugin? Your answer is kept for this exact file.";
  return MessageBoxW(nullptr, widen(text).c_str(), L"" PRODUCT_NAME ": fighter plugin",
                     MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND | MB_TASKMODAL) == IDYES;
#else
  (void)p;
  return false;
#endif
}

// ---- command line ----
namespace {
struct Command {
  std::string settings_path = "port-settings.ini";
  std::string fighters_dir;
  bool fighters_dir_set = false, vanilla = false;
  std::vector<std::pair<std::string, std::string>> actions;   // option, operand
  std::string problem;
};

Command parse(const std::vector<std::string>& args) {
  Command c;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const bool has_value = i + 1 < args.size();
    if (a == "--settings-path" && has_value) c.settings_path = args[++i];
    else if (a == "--fighters-dir" && has_value) { c.fighters_dir = args[++i]; c.fighters_dir_set = true; }
    else if (a == "--vanilla-only" || a == "--vanilla-game") c.vanilla = true;
    else if (a == "--list-plugins") c.actions.push_back({a, ""});
    else if (a == "--approve-plugin" || a == "--revoke-plugin") {
      if (!has_value) c.problem = a + " needs a value";
      else c.actions.push_back({a, args[++i]});
    }
  }
  if (c.problem.empty() && c.actions.size() != 1)
    c.problem = "give one of --approve-plugin <file>, --revoke-plugin <sha256>, --list-plugins";
  return c;
}

// The folder a run with these options takes plugins from; empty for none.
fs::path plugin_folder(const Command& c, const fs::path& program_dir) {
  if (c.vanilla) return {};
  if (c.fighters_dir_set) return c.fighters_dir == "none" ? fs::path() : fs::u8path(c.fighters_dir);
  return default_plugin_folder(program_dir);
}
}  // namespace

bool cli_requested(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--approve-plugin" || a == "--list-plugins" || a == "--revoke-plugin") return true;
  }
  return false;
}

int cli_run(int argc, char** argv, const fs::path& program_dir, std::string* out) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  const Command c = parse(args);
  json j;
  int code = 0;
  const fs::path store_file = store_path(c.settings_path);
  try {
    if (!c.problem.empty()) {
      j = {{"ok", false}, {"error", c.problem}};
      code = 2;
    } else if (c.actions[0].first == "--approve-plugin") {
      j["action"] = "approve";
      std::error_code ec;
      fs::path path = fs::u8path(c.actions[0].second);
      if (!path.is_absolute()) path = fs::absolute(path, ec);
      Plugin p = examine(path, load_store(store_file), builtin_keys());
      if (!p.problem.empty()) {
        j["ok"] = false;
        j["error"] = p.path.u8string() + ": " + p.problem;
        code = 1;
      } else if (!looks_like_plugin(p.bytes)) {
        j["ok"] = false;
        j["error"] = p.path.u8string() + " is not a fighter plugin";
        code = 1;
      } else {
        std::string error;
        const std::string date = now_utc();
        if (!update_store(store_file, [&](Store& s) { s.entries[p.sha256] = {true, p.name, p.file, date}; }, &error)) {
          j["ok"] = false;
          j["error"] = error;
          code = 1;
        } else {
          p.approval = 1;
          p.approval_date = date;
          if (p.status == Status::Blocked) p.status = Status::Approved;
          j["ok"] = true;
          j["plugin"] = plugin_json(p);
        }
      }
    } else if (c.actions[0].first == "--revoke-plugin") {
      j["action"] = "revoke";
      const std::string sha = lower(c.actions[0].second);
      if (!is_sha256_hex(sha)) {
        j["ok"] = false;
        j["error"] = "--revoke-plugin needs the plugin's SHA-256 (64 hex digits, as --list-plugins prints it)";
        code = 2;
      } else {
        std::string error, was = "none";
        const std::string date = now_utc();
        const bool saved = update_store(store_file, [&](Store& s) {
          const auto it = s.entries.find(sha);
          Approval a;
          if (it != s.entries.end()) {
            was = it->second.approved ? "approved" : "refused";
            a = it->second;
          }
          a.approved = false;
          a.date = date;
          s.entries[sha] = a;
        }, &error);
        j["ok"] = saved;
        j["sha256"] = sha;
        j["was"] = was;
        if (!saved) {
          j["error"] = error;
          code = 1;
        }
      }
    } else {
      j["action"] = "list";
      const Store store = load_store(store_file);
      const fs::path folder = plugin_folder(c, program_dir);
      json plugins = json::array();
      if (!folder.empty())
        for (const Plugin& p : scan(folder, store, builtin_keys())) plugins.push_back(plugin_json(p));
      json approvals = json::array();
      for (const auto& [sha, a] : store.entries)
        approvals.push_back({{"sha256", sha}, {"approved", a.approved}, {"name", a.name}, {"file", a.file}, {"date", a.date}});
      j["ok"] = true;
      j["folder"] = folder.empty() ? json() : json(folder.u8string());
      j["plugins"] = plugins;
      j["approvals"] = approvals;
      j["approvals_error"] = store.error.empty() ? json() : json(store.error);
    }
  } catch (const std::exception& e) {
    j = {{"ok", false}, {"error", std::string("unexpected: ") + e.what()}};
    code = 1;
  }
  j["approvals_file"] = store_file.u8string();
  j["signing_keys"] = builtin_keys().size();
  j["test_key"] = builtin_test_key();
  *out = json_text(j) + "\n";
  return code;
}

std::string print_info_fields(const fs::path& program_dir) {
  try {
    std::vector<std::string> args;
#ifdef _WIN32
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string a((size_t)std::max(n - 1, 0), '\0');
      if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a.data(), n, nullptr, nullptr);
      args.push_back(a);
    }
    if (argv) LocalFree(argv);
#endif
    Command c = parse(args);
    const fs::path store_file = store_path(c.settings_path);
    const fs::path folder = plugin_folder(c, program_dir);
    const Store store = load_store(store_file);
    json blocked = json::array();
    if (!folder.empty()) {
      for (const Plugin& p : scan(folder, store, builtin_keys())) {
        if (p.problem.empty() && p.status != Status::Blocked) continue;
        json entry = {{"file", p.file}, {"name", p.name}};
        if (!p.problem.empty()) entry["error"] = p.problem;
        else {
          entry["sha256"] = p.sha256;
          entry["signature"] = signature_name(p.signature);
          entry["approval"] = approval_name(p.approval);
        }
        blocked.push_back(entry);
      }
    }
    std::string out = ",\"plugin_signing_keys\":" + std::to_string(builtin_keys().size());
    out += ",\"plugin_test_key\":" + std::string(builtin_test_key() ? "true" : "false");
    out += ",\"plugin_approvals\":" + json_text(json(store_file.u8string()));
    out += ",\"plugins_folder\":" + (folder.empty() ? std::string("null") : json_text(json(folder.u8string())));
    out += ",\"plugins_blocked\":" + json_text(blocked);
    return out;
  } catch (...) {
    return ",\"plugins_blocked\":null";
  }
}

}  // namespace plugin_gate
