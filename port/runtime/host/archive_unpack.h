// The one archive reader of the host: every zip and .tar.gz this program reads (mod imports, the Mods
// menu's downloads and exports, opponents' skins, one-click looks, optional downloads) goes through
// here, with one set of rules, so bad input fails the same way everywhere. Read in process: no
// tar.exe (Wine and Proton have none, and some Windows installs cannot start it).
//
// Opening a zip checks all of it before a byte is handed out: the end record and central directory
// (no zip64, no multi-disk, no trailing records), each entry's local header (same name and method),
// the entry count, each entry's size and the total against the caller's Limits, the compression
// ratio, entries whose data overlaps (a 17 KB zip once unpacked to 335 MB), encrypted entries,
// methods other than stored and deflate, symbolic links, names that are absolute, hold a drive,
// a backslash or "." / ".." parts (safe_name), and two names that differ only in case. One thread
// at a time per Zip. Each entry read is
// inflated into a buffer of its declared size and its CRC checked. Callers get bytes in memory
// (Zip::read) or an extraction into a folder they name (Zip::extract, extract); nothing else.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace host::archive {

// The most one archive may hold. An archive past any of these is refused when it is opened.
struct Limits {
  uint32_t max_entries = 4096;
  uint64_t max_entry_bytes = 64ull << 20;     // one entry, unpacked
  uint64_t max_total_bytes = 512ull << 20;    // every entry, unpacked
  // Unpacked size over packed size, for an entry of more than 1 MiB. 0: not checked, only for an
  // archive the caller has already matched to a pinned SHA-256 (its bytes are known).
  uint32_t max_ratio = 250;
};
// A mod the player imports or the Mods menu downloads (cosmetic_mods.h kMaxAssetBytes,
// kMaxArchiveBytes, kMaxArchiveEntries).
inline constexpr Limits kModLimits{4096, 64ull << 20, 512ull << 20, 250};
// A download whose whole file was matched to its pinned SHA-256 before it is unpacked (optional
// downloads, one-click looks, the patch tool).
inline constexpr Limits kPinnedLimits{65535, 1ull << 30, 4ull << 30, 0};

// One entry of a zip, as its central directory gives it (checked against its local header).
struct Entry {
  std::string name;               // '/' between folders; a folder's own entry ends in '/'
  uint64_t size = 0;              // unpacked
  uint64_t packed = 0;
  uint32_t crc = 0;
  uint16_t method = 0;            // 0 stored, 8 deflate
  uint16_t flags = 0;
  uint64_t data = 0;              // where its data starts in the archive
  bool folder() const { return !name.empty() && name.back() == '/'; }
  // The file name's extension in lower case (".dat"; empty for none), read from the name's bytes: a
  // zip may name its files in a legacy code page, not UTF-8, and std::filesystem's UTF-8 conversion
  // (u8path) throws on such a name. Use this, not a path, to sort entries read in memory.
  std::string extension() const;
};

class Zip {
 public:
  // A zip file, or a zip already in memory (the bytes are kept). False, with the reason in `error`
  // (plain text for a log or the player), when any rule above is broken.
  bool open(const std::filesystem::path& file, const Limits& limits, std::string* error);
  bool open_memory(std::vector<uint8_t> bytes, const Limits& limits, std::string* error);
  const std::vector<Entry>& entries() const { return entries_; }
  // The entry of exactly that name, or null.
  const Entry* find(const std::string& name) const;
  // One entry's bytes, CRC checked. `entry` is one of entries() (or a copy of one).
  bool read(const Entry& entry, std::vector<uint8_t>* out, std::string* error) const;
  // Every entry under `dest`. Names Windows cannot hold as files (reserved device names, a part
  // ending in a dot or a space, < > | or DEL, text that is not UTF-8) are refused first, so nothing is written
  // for such an archive. `unwritable` (optional) gets the file that could not be written.
  bool extract(const std::filesystem::path& dest, std::string* error, std::string* unwritable = nullptr) const;

 private:
  bool parse(uint64_t total, const Limits& limits, std::string* error);
  bool read_at(uint64_t offset, void* out, uint64_t size) const;
  std::filesystem::path file_;
  mutable std::ifstream stream_;   // the open file (open), kept for the reads
  std::vector<uint8_t> memory_;
  bool in_memory_ = false;
  std::vector<Entry> entries_;
};

// A downloaded .zip or .tar.gz (told apart by its first bytes) unpacked under `dest`, by the same
// rules and `limits`. A .tar.gz's links are skipped, never made (the Diet Melee patcher has some).
// False with the reason in `error`; `unwritable` (optional) gets the file that could not be written
// (empty: the archive itself was refused).
bool extract(const std::filesystem::path& archive, const std::filesystem::path& dest, const Limits& limits,
             std::string* error, std::string* unwritable = nullptr);

// A name from an archive that stays inside the folder it would be unpacked to: relative, '/'
// between parts, no drive, colon or backslash, no "." / ".." or empty part, no control characters
// or " * ?, at most 1024 bytes. A folder's own name may end in one '/'. (The same rule the importer
// always had; extract() adds the names Windows cannot hold.)
bool safe_name(const std::string& name);

// A finished file into place beside the game (rename, else copy), tried for about 5 s while antivirus
// holds it; a whole folder the same way, file by file when a rename fails; and the plain message for
// a failed write there (Windows' reason, what to do about it).
bool place_file(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);
bool place_folder(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);
std::string place_error(const std::string& what, const std::filesystem::path& dir, const std::error_code& ec);
}  // namespace host::archive
