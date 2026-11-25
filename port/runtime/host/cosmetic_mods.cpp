// Native cosmetic catalog, import, and file-aware disc overrides: the catalog's state, the shared
// helpers for bytes, files, hashes and zips, and the test hooks. The parts are in cosmetic_*.cpp;
// cosmetic_mods_internal.h lists what they share.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

std::mutex g_mutex;
fs::path g_root;
std::vector<AssetRecord> g_assets;
std::vector<DiscScan> g_disc_scans;
Profile g_profile;
bool g_configured = false;
bool g_catalog_valid = true;
bool g_profile_valid = true;
std::string g_message;
std::shared_ptr<const RuntimeState> g_runtime = std::make_shared<RuntimeState>();
std::atomic<uint32_t> g_online_freezes{0};
// The mod disc this session serves as its own files (set_session_pack), spelled as a scan's key.
std::string g_session_pack;
// The exact look in force ("Diet Classic", "Diet 64", "Crystal") or empty: every file of the pack scanned
// under that name is served while it is set (publish_locked), on top of the player's own picks.
std::string g_look_pack;
// The disc whose alternate stages are this session's stages (set_session_stage_pack), as a scan's key.
std::string g_session_stage_pack;
// Buttons held on any controller, asked when a match's stage is planned (set_stage_button_probe).
uint32_t (*g_stage_buttons)() = nullptr;
// A lockstep session is running (set_lockstep_probe).
bool (*g_lockstep)() = nullptr;
// Newly built packs have been hash-verified, but are scanned for assets at the next start.
bool g_look_prepared_this_run = false;

uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}
uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
void put_be32(uint8_t* p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

std::string lower(std::string value) {
  for (char& c : value) c = (char)std::tolower((unsigned char)c);
  return value;
}

// A DAT archive is named .dat, or .usd when it is the English variant of a file the disc keeps
// per language (Captain Falcon's red costume, Pokemon Stadium). A mod ships under either name.
bool dat_extension(const std::string& name) {
  const std::string extension = lower(fs::path(name).extension().string());
  return extension == ".dat" || extension == ".usd";
}


// The length every random pick of a stage is served at (the longest eligible skin in either form),
// by target file. The game can keep a stage file's length from an earlier pick (a file read ahead
// before this match's pick was published), so with one length for every pick a smaller skin no longer
// ends before the reads the game makes ("disc read failed" in the middle of Final Destination).
std::map<std::string, size_t> g_stage_extent;

std::string wide_to_utf8(const std::wstring& value) {
  if (value.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(),
                              nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), out.data(), n,
                      nullptr, nullptr);
  return out;
}

std::string path_filename_utf8(const fs::path& path) {
  return wide_to_utf8(path.filename().wstring());
}

bool safe_relative_path(const std::string& raw) {
  if (raw.empty() || raw.size() > 1024 || raw.front() == '/' || raw.front() == '\\') return false;
  if (raw.find('\\') != std::string::npos || raw.find(':') != std::string::npos ||
      raw.find('"') != std::string::npos || raw.find('*') != std::string::npos ||
      raw.find('?') != std::string::npos) return false;
  std::string component;
  for (size_t i = 0; i <= raw.size(); ++i) {
    char c = i == raw.size() ? '/' : raw[i];
    if ((unsigned char)c < 0x20) return false;
    if (c == '/') {
      // A single trailing slash is accepted for a directory entry. Empty interior components,
      // dot components, and parent traversal are rejected on every platform.
      if (component.empty()) return i == raw.size() && i > 0 && raw[i - 1] == '/';
      if (component == "." || component == "..") return false;
      component.clear();
    } else component += c;
  }
  return true;
}

bool read_bounded(const fs::path& path, uint64_t limit, std::vector<uint8_t>* out,
                  std::string* error) {
  std::error_code ec;
  uint64_t size = fs::file_size(path, ec);
  if (ec) { *error = "Cannot read " + path_filename_utf8(path) + "."; return false; }
  if (!size) { *error = "The selected file is empty."; return false; }
  if (size > limit) {
    *error = "The selected resource is larger than the " + std::to_string(limit / (1024 * 1024)) + " MB safety limit.";
    return false;
  }
  out->resize((size_t)size);
  std::ifstream file(path, std::ios::binary);
  if (!file || !file.read((char*)out->data(), (std::streamsize)out->size())) {
    out->clear(); *error = "The selected file could not be read completely."; return false;
  }
  return true;
}

bool file_stamp(const fs::path& path, uint64_t* size, int64_t* mtime) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) return false;
  *size = fs::file_size(path, ec);
  if (ec) return false;
  *mtime = (int64_t)fs::last_write_time(path, ec).time_since_epoch().count();
  return !ec;
}

// A bounded read of one file inside a disc image (or of a whole loose file, offset 0).
bool read_range(const fs::path& path, uint64_t offset, uint64_t length, std::vector<uint8_t>* out) {
  out->clear();
  if (!length || length > kMaxAssetBytes) return false;
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || offset > size || length > size - offset) return false;
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg((std::streamoff)offset);
  out->resize((size_t)length);
  if (!file.read((char*)out->data(), (std::streamsize)length)) { out->clear(); return false; }
  return true;
}

uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::string sha256(const uint8_t* data, size_t size) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  uint8_t digest[32]{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  NTSTATUS result = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data), (ULONG)size,
                               digest, sizeof digest);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (result < 0) return {};
  std::ostringstream text;
  text << std::hex << std::setfill('0');
  for (uint8_t byte : digest) text << std::setw(2) << (unsigned)byte;
  return text.str();
}

std::string sha256(const std::vector<uint8_t>& data) { return sha256(data.data(), data.size()); }
std::string sha256_text(const std::string& text) {
  return sha256((const uint8_t*)text.data(), text.size());
}

bool write_atomic(const fs::path& path, const uint8_t* data, size_t size, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) { *error = "Cannot create the cosmetic catalog folder."; return false; }
  fs::path temporary = path;
  temporary += L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) { *error = "Cannot create a temporary catalog file."; return false; }
  bool ok = true;
  size_t written_total = 0;
  while (written_total < size) {
    DWORD chunk = (DWORD)std::min<size_t>(size - written_total, 1u << 20);
    DWORD written = 0;
    if (!WriteFile(file, data + written_total, chunk, &written, nullptr) || written != chunk) {
      ok = false; break;
    }
    written_total += written;
  }
  if (ok) ok = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  // A scanner or sync tool that has just opened the catalog makes the replace fail with a sharing
  // or access error for a moment (a player's every skin change on character select failed this
  // way). Ask again for up to about half a second before giving up.
  for (int attempt = 0; ok; ++attempt) {
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
    const DWORD why = GetLastError();
    if (attempt >= 10 || (why != ERROR_ACCESS_DENIED && why != ERROR_SHARING_VIOLATION && why != ERROR_LOCK_VIOLATION)) ok = false;
    else Sleep(50);
  }
  if (!ok) {
    DeleteFileW(temporary.c_str());
    *error = "The cosmetic catalog could not be committed atomically.";
  }
  return ok;
}

bool write_atomic(const fs::path& path, const std::string& text, std::string* error) {
  return write_atomic(path, (const uint8_t*)text.data(), text.size(), error);
}

json string_array(const std::vector<std::string>& values) {
  json out = json::array();
  for (const auto& value : values) out.push_back(value);
  return out;
}

std::vector<std::string> get_string_array(const json& object, const char* key) {
  std::vector<std::string> out;
  auto it = object.find(key);
  if (it == object.end() || !it->is_array()) return out;
  for (const auto& value : *it) if (value.is_string()) out.push_back(value.get<std::string>());
  return out;
}

// A zip the player picked (or a Nucleus vault), opened by the host's one zip reader with the mod
// limits (archive_unpack.h: at most 4096 entries, 64 MB each, 512 MB in all, none packed more than
// 250 to 1; unsafe or repeated names, links and encryption refused before a byte is read). Its
// entries are then read in process with Zip::read, CRC checked.
bool open_zip(const fs::path& path, Zip* zip, std::string* error) {
  std::error_code ec;
  const uint64_t file_size = fs::file_size(path, ec);
  if (ec) { *error = "The ZIP archive could not be opened."; return false; }
  if (!file_size || file_size > kMaxArchiveBytes) {
    *error = "The ZIP archive is empty or larger than the 512 MB safety limit."; return false;
  }
  return zip->open(path, host::archive::kModLimits, error);
}

std::wstring quote_windows_arg(const std::wstring& arg) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'\"') { out.append(slashes * 2 + 1, L'\\'); out += L'\"'; slashes = 0; continue; }
    out.append(slashes, L'\\'); slashes = 0; out += c;
  }
  out.append(slashes * 2, L'\\'); out += L'\"';
  return out;
}
bool family_has_color(const FighterFamily& family, const char* code) {
  return (std::string(" ") + family.colors + " ").find(std::string(" ") + code + " ") != std::string::npos;
}
std::string slot_file(const SlotName& slot) {
  return std::string("Pl") + slot.family->file_code + slot.color->code + ".dat";
}
bool find_slot(const std::string& target_path, SlotName* out) {
  const std::string wanted = lower(target_path);
  for (const auto& family : families)
    for (const auto& color : color_names) {
      if (!family_has_color(family, color.code)) continue;
      const SlotName slot{&family, &color};
      if (lower(slot_file(slot)) == wanted) { *out = slot; return true; }
    }
  return false;
}

}  // namespace cosmetic_internal

namespace testing {
bool apply_look_patch(const std::string& tool, const std::string& source, const std::string& patch,
                     const std::string& output, std::string* error) {
#ifdef _WIN32
  return run_xdelta(fs::u8path(tool), fs::u8path(source), fs::u8path(patch), fs::u8path(output), error);
#else
  (void)tool; (void)source; (void)patch; (void)output;
  *error = "The Windows patch-tool diagnostic is not available on this host."; return false;
#endif
}
DatInspection inspect_dat(const std::vector<uint8_t>& bytes) { return inspect_dat_impl(bytes); }
bool look_file_safe(const std::string& path, const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack,
                    std::string* why) {
  std::string local; if (!why) why = &local;
  return host::cosmetics::look_file_safe(path, clean, pack, why);
}
bool effect_particles_same(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack) {
  return host::cosmetics::effect_particles_same(clean, pack);
}
bool costume_structure_safe(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>* standard, std::string* why) {
  std::string local; if (!why) why = &local;
  return host::cosmetics::costume_structure_safe(bytes, standard, why);
}
bool stage_structure_safe(const std::vector<uint8_t>& bytes, std::string* why) {
  std::string local; if (!why) why = &local;
  return host::cosmetics::stage_structure_safe(bytes, why);
}
bool parse_bank(const std::vector<uint8_t>& bytes, std::string* error) {
  std::string local; if (!error) error = &local;
  BankLayout layout;
  return host::cosmetics::parse_bank(bytes, &layout, error);
}
bool visual_dat_only(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                     std::string* error) {
  std::string local; if (!error) error = &local;
  return host::cosmetics::visual_dat_only(clean, candidate, error);
}
bool costume_skeleton_matches(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                              std::string* error) {
  std::string local; if (!error) error = &local;
  return host::cosmetics::costume_skeleton_matches(clean, candidate, error);
}
bool materialize_effect_dat(const std::string& target_path,
                            const std::vector<uint8_t>& clean,
                            const std::vector<uint8_t>& candidate,
                            std::vector<uint8_t>* runtime,
                            std::string* classification,
                            std::string* error) {
  std::string local; if (!error) error = &local;
  return materialize_effect_dat_impl(target_path, clean, candidate, runtime, classification, error);
}
bool portrait_slot_from_name(const std::string& name, std::string* slot, std::string* kind) {
  return portrait_slot_from_name_impl(name, slot, kind);
}
bool inspect_bank(const std::vector<uint8_t>& bytes, std::string* bank, std::string* language, std::string* error) {
  std::string local; if (!error) error = &local;
  VoiceCheck check;
  if (!check_voice(bytes, {}, {}, &check, error)) return false;
  if (bank) *bank = check.bank->file;
  if (language) *language = kBankLanguages[check.language];
  return true;
}
bool inspect_zip(const std::string& path, std::vector<std::string>* names, std::string* error) {
  Zip zip;
  if (!open_zip(fs::u8path(path), &zip, error)) return false;
  names->clear(); for (const auto& entry : zip.entries()) names->push_back(entry.name);
  return true;
}
bool merge_fighter_sounds(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& mod,
                          size_t fighter_kind, std::vector<uint8_t>* merged, std::string* error) {
  std::string local; if (!error) error = &local;
  return fighter_sound_overlay(clean, mod, fighter_kind, merged, error);
}
}  // namespace testing

}  // namespace host::cosmetics
