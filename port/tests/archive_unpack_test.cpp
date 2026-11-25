// The host's one archive reader (archive_unpack.h): zips and .tar.gz made here by Windows' own tar,
// and zips made by hand (traversal, a bomb, a repeated name, a cut-off file, shared data, a link, a
// bad checksum), read without tar.exe (Wine and Proton have none), with the caller's limits.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "archive_unpack.h"
#include "optional_downloads.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>

// The test's own deflate, for the bomb (the game only inflates).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"

namespace host {
void log(const char*, ...) {}
void request_exit(int) {}
}

namespace {
namespace fs = std::filesystem;
using host::archive::Limits;
using host::archive::Zip;

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}
// An archive of the given members made by Windows' own tar (the test's tool only; the game reads them itself).
bool make_zip(const fs::path& folder, const fs::path& zip, const std::wstring& members) {
  std::wstring cmd = L"tar.exe -a -c -f \"" + zip.wstring() + L"\" " + members;
  STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      folder.wstring().c_str(), &si, &pi)) return false;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  return code == 0;
}
bool extract(const fs::path& archive, const fs::path& dest, const Limits& limits = host::archive::kModLimits,
             std::string* why = nullptr) {
  std::string error;
  const bool ok = host::archive::extract(archive, dest, limits, &error);
  if (why) *why = error;
  return ok;
}
std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// --download <fsr> <game folder>: the real download and install of one optional
// part, as the game's buttons run it (network; not part of ctest). Prints the outcome and its message.
int run_download(const std::string& part, const std::string& dir) {
  using namespace host::downloads;
  struct Part { const char* name; void (*start)(const std::string&); ModelState (*state)(const std::string&); std::string (*message)(); };
  const Part parts[] = {
      {"fsr", fsr_download, fsr_state, fsr_message}};
  for (const Part& p : parts) {
    if (part != p.name) continue;
    p.start(dir);
    Sleep(100);
    std::string last;
    while (p.state(dir) == ModelState::Downloading) {
      const std::string m = p.message();
      if (m != last) { std::printf("  %s\n", m.c_str()); last = m; }
      Sleep(250);
    }
    const ModelState s = p.state(dir);
    std::printf("%s: %s | %s\n", p.name, s == ModelState::Present ? "INSTALLED" : s == ModelState::Failed ? "FAILED" : "MISSING",
                p.message().c_str());
    shutdown();
    return s == ModelState::Present ? 0 : 1;
  }
  std::printf("unknown part %s\n", part.c_str());
  return 2;
}

// A zip made by hand. Each member: its name, its bytes, and how they are stored.
struct Member {
  std::string name;
  std::string data;          // unpacked
  bool deflate = false;
  int shares = -1;           // >= 0: no data of its own, it points at that member's
  uint32_t unix_mode = 0;    // the mode a zip made on Unix records (0120000: a symbolic link)
  bool bad_crc = false;
};
void u16(std::string& s, uint32_t v) { s += (char)(v & 0xFF); s += (char)(v >> 8 & 0xFF); }
void u32(std::string& s, uint32_t v) { u16(s, v & 0xFFFF); u16(s, v >> 16); }
uint32_t crc_of(const std::string& d) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : d) { c ^= b; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
  return c ^ 0xFFFFFFFFu;
}
std::string deflated(const std::string& data) {
  int n = 0;
  unsigned char* z = stbi_zlib_compress((unsigned char*)data.data(), (int)data.size(), &n, 9);
  // zlib's 2-byte header and 4-byte Adler-32 off: a zip holds the raw deflate stream.
  std::string raw = z && n > 6 ? std::string((const char*)z + 2, (size_t)n - 6) : std::string();
  std::free(z);
  return raw;
}
std::string make_zip(const std::vector<Member>& members) {
  std::string out, dir;
  std::vector<uint32_t> offsets(members.size());
  std::vector<std::string> packed(members.size());
  for (size_t i = 0; i < members.size(); ++i) {
    const Member& m = members[i];
    packed[i] = m.deflate ? deflated(m.data) : m.data;
    if (m.shares >= 0) { offsets[i] = offsets[(size_t)m.shares]; continue; }
    offsets[i] = (uint32_t)out.size();
    u32(out, 0x04034b50u); u16(out, 20); u16(out, 0); u16(out, m.deflate ? 8 : 0); u16(out, 0); u16(out, 0);
    u32(out, crc_of(m.data) ^ (m.bad_crc ? 1u : 0u)); u32(out, (uint32_t)packed[i].size()); u32(out, (uint32_t)m.data.size());
    u16(out, (uint32_t)m.name.size()); u16(out, 0);
    out += m.name + packed[i];
  }
  for (size_t i = 0; i < members.size(); ++i) {
    const Member& m = members[i];
    u32(dir, 0x02014b50u); u16(dir, m.unix_mode ? (3u << 8) | 20 : 20); u16(dir, 20); u16(dir, 0); u16(dir, m.deflate ? 8 : 0);
    u16(dir, 0); u16(dir, 0);
    u32(dir, crc_of(m.data) ^ (m.bad_crc ? 1u : 0u)); u32(dir, (uint32_t)packed[i].size()); u32(dir, (uint32_t)m.data.size());
    u16(dir, (uint32_t)m.name.size()); u16(dir, 0); u16(dir, 0); u16(dir, 0); u16(dir, 0); u32(dir, m.unix_mode << 16);
    u32(dir, offsets[i]);
    dir += m.name;
  }
  std::string eocd;
  u32(eocd, 0x06054b50u); u16(eocd, 0); u16(eocd, 0); u16(eocd, (uint32_t)members.size()); u16(eocd, (uint32_t)members.size());
  u32(eocd, (uint32_t)dir.size()); u32(eocd, (uint32_t)out.size()); u16(eocd, 0);
  return out + dir + eocd;
}
// Opened in memory with the mod limits: "" when it opens, else the reason.
std::string refusal(const std::string& zip, const Limits& limits = host::archive::kModLimits) {
  Zip z;
  std::string why;
  return z.open_memory(bytes_of(zip), limits, &why) ? std::string() : (why.empty() ? std::string("(no reason)") : why);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--download") return run_download(argv[2], argv[3]);
  std::error_code ec;
  const fs::path root = fs::temp_directory_path() / ("mu-unpack-test-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(root, ec);
  fs::create_directories(root / "Part" / "Sys", ec);
  // A deflated zip made by tar, unpacked in process byte for byte, and read in memory.
  {
    { std::ofstream(root / "Part" / "Sys" / "codehandler.bin") << "x"; }
    { std::ofstream big(root / "Part" / "Sys" / "big.bin", std::ios::binary); for (int i = 0; i < 200000; ++i) big << (char)(i * 7 % 251); }
    const fs::path packed = root / "packed.zip", out = root / "unpacked";
    check(make_zip(root, packed, L"Part"), "compressed test archive made");
    check(extract(packed, out), "the archive unpacks without tar");
    std::ifstream in(out / "Part" / "Sys" / "big.bin", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), {});
    bool same = got.size() == 200000;
    for (int i = 0; same && i < 200000; ++i) same = got[(size_t)i] == (char)(i * 7 % 251);
    check(same, "an unpacked file matches the original byte for byte");
    {
      Zip zip;
      std::string why;
      std::vector<uint8_t> member;
      const host::archive::Entry* e = zip.open(packed, host::archive::kModLimits, &why) ? zip.find("Part/Sys/big.bin") : nullptr;
      check(e && zip.read(*e, &member, &why) && member.size() == 200000 && member[1234] == (uint8_t)(1234 * 7 % 251),
            "a member is read in memory by its name");
    }
    check(!extract(root / "missing.zip", out), "a missing archive does not unpack");
    check(!extract(root / "Part" / "Sys" / "big.bin", out), "a file that is not a zip does not unpack as one");
  }
  // Mod downloads (.zip and .tar.gz): pax (Windows' tar) and GNU long names, and nothing may land
  // outside the folder.
  {
    const std::string deep(120, 'd');
    fs::create_directories(root / "Mod" / deep, ec);
    { std::ofstream(root / "Mod" / deep / "patch.xdelta", std::ios::binary) << "patch bytes"; }
    { std::ofstream big(root / "Mod" / "big.bin", std::ios::binary); for (int i = 0; i < 300000; ++i) big << (char)(i * 13 % 241); }
    const fs::path pax = root / "pax.tar.gz", gnu = root / "gnu.tar.gz", zip = root / "mod.zip";
    check(make_zip(root, pax, L"--format pax -z Mod") && make_zip(root, gnu, L"--format gnutar -z Mod") && make_zip(root, zip, L"Mod"),
          "mod test archives made");
    for (const fs::path& archive : {pax, gnu, zip}) {
      const fs::path out = root / ("mod-" + archive.stem().string());
      const bool unpacked = extract(archive, out);
      std::ifstream patch(out / "Mod" / deep / "patch.xdelta", std::ios::binary), big(out / "Mod" / "big.bin", std::ios::binary);
      const std::string p((std::istreambuf_iterator<char>(patch)), {}), b((std::istreambuf_iterator<char>(big)), {});
      bool same = b.size() == 300000;
      for (int i = 0; same && i < 300000; ++i) same = b[(size_t)i] == (char)(i * 13 % 241);
      check(unpacked && p == "patch bytes" && same, "a mod download unpacks without tar: " + archive.filename().string());
    }
    { std::ofstream(root / "evil.txt") << "x"; }
    const fs::path evil = root / "evil.tar.gz";
    check(make_zip(root / "Mod", evil, L"-z -P ../evil.txt"), "an archive leaving its folder made");
    check(!extract(evil, root / "mod-evil" / "in") && !fs::exists(root / "mod-evil" / "evil.txt"), "a .tar.gz entry outside the folder is refused");
    check(!extract(root / "missing.tar.gz", root / "x"), "a missing download does not unpack");
    // The caller's limits: entries, and the bytes the entries declare, refused before anything is written.
    const Limits few_entries{2, 1ull << 30, 1ull << 30, 250}, few_bytes{64, 1ull << 30, 100000, 250}, small_entry{64, 100000, 1ull << 30, 250};
    check(!extract(zip, root / "mod-few-entries", few_entries) && !fs::exists(root / "mod-few-entries" / "Mod" / "big.bin"),
          "a zip with more entries than allowed is refused");
    check(!extract(zip, root / "mod-few-bytes", few_bytes) && !fs::exists(root / "mod-few-bytes" / "Mod" / deep / "patch.xdelta"),
          "a zip larger than allowed unpacked is refused");
    check(!extract(zip, root / "mod-small-entry", small_entry) && !fs::exists(root / "mod-small-entry" / "Mod" / deep / "patch.xdelta"),
          "a zip with one entry larger than allowed is refused");
    check(!extract(pax, root / "mod-few-bytes-tar", few_bytes), "a .tar.gz larger than allowed unpacked is refused");
    check(extract(zip, root / "mod-in-limits", {64, 1ull << 20, 1ull << 20, 250}), "a zip within its limits unpacks");
  }
  // Zips made by hand: what a hostile or damaged download looks like.
  {
    const std::string block(4096, 'z');
    const std::string good = make_zip({{"a.dat", block}, {"b.dat", block}});
    check(refusal(good).empty(), "a zip made by hand opens");
    {
      const fs::path distinct = root / "distinct.zip";
      { std::ofstream(distinct, std::ios::binary) << good; }
      check(extract(distinct, root / "zip-distinct") && fs::file_size(root / "zip-distinct" / "b.dat", ec) == 4096, "a zip made by hand unpacks");
    }
    // Traversal: a name leaving the folder, an absolute name, a drive, a backslash.
    for (const std::string& name : {std::string("../up.dat"), std::string("in/../../up.dat"), std::string("/abs.dat"),
                                    std::string("C:/drive.dat"), std::string("dir\\back.dat"), std::string("./here.dat")}) {
      const std::string why = refusal(make_zip({{"ok.dat", block}, {name, block}}));
      check(why.find("unsafe name") != std::string::npos, "a zip naming " + name + " is refused (" + why + ")");
    }
    {
      const fs::path escape = root / "escape.zip";
      { std::ofstream(escape, std::ios::binary) << make_zip({{"ok.dat", block}, {"../up.dat", block}}); }
      check(!extract(escape, root / "zip-escape" / "in") && !fs::exists(root / "zip-escape" / "up.dat") &&
                !fs::exists(root / "zip-escape" / "in" / "ok.dat"),
            "a zip with an entry outside the folder is refused before anything is written");
    }
    // A bomb: 16 MiB of zeros, which zlib (Windows' tar) packs about 1000 to 1.
    {
      fs::create_directories(root / "Bomb", ec);
      { std::ofstream zeros(root / "Bomb" / "zeros.dat", std::ios::binary); const std::string mb(1u << 20, '\0'); for (int i = 0; i < 16; ++i) zeros << mb; }
      const fs::path bomb = root / "bomb.zip";
      check(make_zip(root / "Bomb", bomb, L"zeros.dat"), "a bomb made");
      std::string why;
      const bool refused = !Zip().open(bomb, host::archive::kModLimits, &why);
      check(refused && why.find("250 to 1") != std::string::npos, "a zip packing one entry far past 250 to 1 is refused (" + why + ")");
      check(!extract(bomb, root / "zip-bomb") && !fs::exists(root / "zip-bomb" / "zeros.dat"), "a bomb unpacks nothing");
      check(Zip().open(bomb, {16, 64ull << 20, 64ull << 20, 0}, &why), "the same zip opens when the ratio is not checked (a pinned download)");
      // stb's own deflate packs zeros about 158 to 1: under the mod limit, over a caller's 100.
      const std::string zeros(8u << 20, '\0');
      const std::string packed = make_zip({{"zeros.dat", zeros, true}});
      check(refusal(packed).empty(), "an entry packed 158 to 1 opens with the mod limits");
      check(refusal(packed, {16, 64ull << 20, 64ull << 20, 100}).find("100 to 1") != std::string::npos, "the caller's own ratio is kept");
      Zip z;
      std::string e;
      std::vector<uint8_t> out;
      check(z.open_memory(bytes_of(packed), host::archive::kModLimits, &e) && z.read(z.entries()[0], &out, &e) &&
                out.size() == zeros.size() && out[12345] == 0,
            "a deflated entry inflates to its declared size");
      const std::string under = make_zip({{"under.dat", std::string(512u << 10, '\0'), true}});
      check(refusal(under, {16, 64ull << 20, 64ull << 20, 100}).empty(), "an entry under 1 MiB is never taken for a bomb");
    }
    // The same name twice, letter case aside.
    {
      const std::string why = refusal(make_zip({{"Fox.dat", block}, {"fox.DAT", block}}));
      check(why.find("twice") != std::string::npos, "a zip naming one file twice is refused (" + why + ")");
      check(!refusal(make_zip({{"dir/", ""}, {"dir", block}})).empty(), "a folder and a file of one name are refused");
    }
    // Cut short: at the end record, in the central directory, in the data.
    {
      check(refusal(good.substr(0, good.size() - 10)).find("cut short") != std::string::npos, "a zip whose end record is cut is refused");
      check(!refusal(good.substr(0, good.size() - 40)).empty(), "a zip whose central directory is cut is refused");
      check(!refusal(good.substr(0, 3000)).empty(), "a zip cut in its data is refused");
      check(!refusal(std::string()).empty(), "an empty file is refused");
      const fs::path cut = root / "cut.zip";
      { std::ofstream(cut, std::ios::binary) << good.substr(0, good.size() / 2); }
      check(!extract(cut, root / "zip-cut") && !fs::exists(root / "zip-cut" / "a.dat"), "a cut-off zip on disk unpacks nothing");
    }
    // Entries sharing one block of data (a 17 KB zip of this shape once unpacked to 335 MB).
    check(!refusal(make_zip({{"a.dat", block}, {"b.dat", block, false, 0}, {"c.dat", block, false, 0}})).empty(),
          "a zip whose entries share their data is refused");
    // A symbolic link, from a zip made on Unix.
    check(refusal(make_zip({{"link.dat", "../../etc/passwd", false, -1, 0120777}})).find("link") != std::string::npos,
          "a zip holding a symbolic link is refused");
    // A wrong checksum: found when the entry is read, and nothing is left half written.
    {
      const std::string bad = make_zip({{"ok.dat", block}, {"bad.dat", block, true, -1, 0, true}});
      Zip z;
      std::string why;
      std::vector<uint8_t> out;
      const bool caught = z.open_memory(bytes_of(bad), host::archive::kModLimits, &why) && !z.read(z.entries()[1], &out, &why);
      check(caught && out.empty(), "an entry whose CRC does not match is not handed out (" + why + ")");
      const fs::path bad_zip = root / "bad-crc.zip";
      { std::ofstream(bad_zip, std::ios::binary) << bad; }
      check(!extract(bad_zip, root / "zip-bad-crc") && !fs::exists(root / "zip-bad-crc" / "bad.dat"), "a zip with a bad CRC does not unpack");
    }
    // Names Windows cannot hold are refused on extraction only (an import reads them in memory).
    {
      const std::string odd = make_zip({{"con.dat", block}, {"a|b.dat", block}});
      check(refusal(odd).empty(), "a zip with names Windows cannot hold still opens in memory");
      const fs::path odd_zip = root / "odd.zip";
      { std::ofstream(odd_zip, std::ios::binary) << odd; }
      check(!extract(odd_zip, root / "zip-odd"), "a zip with names Windows cannot hold does not unpack");
      check(refusal(make_zip({{"Shiny PokeFloats PkFlt_2][GrPu.dat", block}})).empty(), "brackets in a name are fine");
    }
  }
  // Putting files in place beside the game.
  {
    const fs::path from = root / "place-from.txt", to = root / "place" / "to.txt";
    { std::ofstream(from) << "placed"; }
    fs::create_directories(to.parent_path(), ec);
    std::error_code place_ec;
    check(host::archive::place_file(from, to, place_ec) && fs::exists(to) && !fs::exists(from), "a file is put in place");
    const std::string m = host::archive::place_error("X could not be put", root, std::make_error_code(std::errc::permission_denied));
    check(m.find("Ransomware protection") != std::string::npos, "a refused write says what to do about it");
  }
  fs::remove_all(root, ec);
  std::printf("%s\n", failures ? "unpack checks FAILED" : "unpack checks passed");
  return failures ? 1 : 0;
}
