// Source Port host: the memory card (slot A, a Dolphin-compatible folder of .gci files) and its
// self-test.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// Memory card: slot A is a Dolphin-compatible folder of .gci files. The game-side shim passes
// native pointers here, so directory entries are converted explicitly instead of exposing their
// big-endian on-card representation as a host struct.
namespace {
constexpr int32_t CARD_READY = 0, CARD_NOCARD = -3, CARD_NOFILE = -4, CARD_EXIST = -7,
                  CARD_NOENT = -8, CARD_INSSPACE = -9, CARD_NOPERM = -10,
                  CARD_LIMIT = -11, CARD_NAMETOOLONG = -12, CARD_FATAL = -128;
constexpr uint32_t CARD_SECTOR = 0x2000, CARD_MAX_FILES = 127, CARD_TOTAL_BLOCKS = 2043,
                   CARD_MEM_SIZE_MBIT = 128;

struct SourceCardFile {
  uint8_t dir[64]{};
  std::vector<uint8_t> data;
  std::filesystem::path path;

  std::string name() const {
    return std::string(reinterpret_cast<const char*>(dir + 8), strnlen(reinterpret_cast<const char*>(dir + 8), 32));
  }
  uint16_t blocks() const { return (uint16_t)(((uint16_t)dir[0x38] << 8) | dir[0x39]); }
};

struct SourceCardStat {
  char fileName[32];
  uint32_t length, time;
  uint8_t gameName[4], company[2], bannerFormat;
  uint32_t iconAddr;
  uint16_t iconFormat, iconSpeed;
  uint32_t commentAddr, offsetBanner, offsetBannerTlut, offsetIcon[8], offsetIconTlut, offsetData;
};
static_assert(sizeof(SourceCardStat) == 0x6C, "native CARDStat layout");

std::vector<SourceCardFile*> g_card_files;
std::filesystem::path g_card_dir;
bool g_card_mounted = false;
std::shared_ptr<const std::vector<uint8_t>> g_console_host_snapshot;

uint16_t card_be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
uint32_t card_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void card_put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void card_put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

const uint8_t* card_disk_id() { return reinterpret_cast<const uint8_t*>(MEM1_BASE); }
bool card_same_game(const SourceCardFile& file) { return std::memcmp(file.dir, card_disk_id(), 6) == 0; }
uint32_t card_used_blocks() {
  uint32_t blocks = 0;
  for (const SourceCardFile* file : g_card_files) if (file) blocks += file->blocks();
  return blocks;
}
int32_t card_find(const std::string& name) {
  for (size_t i = 0; i < g_card_files.size(); ++i) {
    if (g_card_files[i] && card_same_game(*g_card_files[i]) && g_card_files[i]->name() == name)
      return (int32_t)i;
  }
  return -1;
}
// Plain ASCII whatever the locale calls a letter: a byte above 0x7F is not valid text for the path
// conversion that follows.
std::string card_safe_name(const SourceCardFile& file) {
  std::string name;
  for (int i = 0; i < 6; ++i) { const unsigned char ch = file.dir[i]; name += (ch < 0x80 && std::isalnum(ch)) ? (char)ch : '_'; }
  name += "-";
  for (char ch : file.name()) {
    const unsigned char u = (unsigned char)ch;
    name += (u < 0x80 && (std::isalnum(u) || ch == '_' || ch == '-' || ch == '.')) ? ch : '_';
  }
  return name + ".gci";
}
// A path as text for the log. Turning a path into narrow text can fail (a folder name the system
// code page cannot hold), and on the Static Recomp that failure inside a save ended the game. The
// save path here stays wide and never ends the game either.
std::string card_shown(const std::filesystem::path& p) {
  try { const auto text = p.u8string(); return std::string(text.begin(), text.end()); }
  catch (...) { return "<name not printable>"; }
}

void card_refresh_snapshot() {
  const auto melee = card_find("SuperSmashBros0110290334");
}
void card_save(SourceCardFile& file) {
  // The session card is a disposable copy. Guests never persist progress; the host's real card
  // is never touched either. Keep identical in-memory CARD results on all seats for lockstep.
  try {
    if (file.path.empty()) file.path = g_card_dir / card_safe_name(file);
    std::filesystem::path temporary = file.path; temporary += L".tmp";
    FILE* out = _wfopen(temporary.c_str(), L"wb");
    if (!out) { host::log("card: cannot write %s", card_shown(file.path).c_str()); return; }
    const bool ok = std::fwrite(file.dir, 1, sizeof file.dir, out) == sizeof file.dir &&
                    std::fwrite(file.data.data(), 1, file.data.size(), out) == file.data.size();
    std::fclose(out);
    std::error_code ec;
    // Another program (a virus scanner, the search indexer) can hold the new file or the old one
    // open for a moment right after it is written, and the replace is refused. Seen about once in
    // ten saves on one PC. Try again for a short while before giving the save up.
    DWORD code = 0;
    for (int attempt = 0; ok && attempt < 20; ++attempt) {
      if (MoveFileExW(temporary.c_str(), file.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { code = 0; break; }
      code = GetLastError();
      if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED && code != ERROR_LOCK_VIOLATION) break;
      Sleep(10);
    }
    if (!ok || code) {
      std::filesystem::remove(temporary, ec);
      host::log("card: failed to save %s (error %lu)", card_shown(file.path).c_str(), (unsigned long)code);
    }
  } catch (const std::exception& error) {
    host::log("card: failed to save %s (%s)", card_safe_name(file).c_str(), error.what());
  }
}
void card_clear_files() {
  for (SourceCardFile* file : g_card_files) delete file;
  g_card_files.clear();
}
void card_mount_files() {
  if (g_card_mounted) return;
  g_card_dir =       g_profile_card.empty() ? std::filesystem::u8path(host::options.card_dir) : g_profile_card;
  std::error_code ec;
  std::filesystem::create_directories(g_card_dir, ec);
  host::backup_card_folder(g_card_dir);
  card_clear_files();
  g_card_files.assign(CARD_MAX_FILES, nullptr);
  size_t slot = 0;
  for (const auto& entry : std::filesystem::directory_iterator(g_card_dir, ec)) {
    if (ec || entry.path().extension() != L".gci" || slot >= CARD_MAX_FILES) continue;

    FILE* in = _wfopen(entry.path().c_str(), L"rb");
    if (!in) continue;
    auto* file = new SourceCardFile;
    bool ok = std::fread(file->dir, 1, sizeof file->dir, in) == sizeof file->dir;
    if (ok) {
      file->data.resize((size_t)file->blocks() * CARD_SECTOR);
      ok = std::fread(file->data.data(), 1, file->data.size(), in) == file->data.size();
    }
    std::fclose(in);
    if (!ok || file->blocks() == 0 || file->blocks() > CARD_TOTAL_BLOCKS) { delete file; continue; }
    file->path = entry.path();
    // Two files with one save name: card_find would use whichever the folder listed first, so a save
    // dropped in beside the player's own (20XX TE has Melee's own save name) could hide it. The boot
    // check (mods::protect_card_folder) moves such files out; if one is still here, the file the game
    // itself writes (card_safe_name) is the one mounted.
    const int32_t same = card_find(file->name());
    if (same >= 0 && std::memcmp(g_card_files[same]->dir, file->dir, 6) == 0) {
      const bool keep_new = file->path.filename() == std::filesystem::u8path(card_safe_name(*file)) &&
                            g_card_files[same]->path.filename() != std::filesystem::u8path(card_safe_name(*g_card_files[same]));
      host::log("card: %s and %s have the same save name; %s is used", g_card_files[same]->path.u8string().c_str(),
                file->path.u8string().c_str(), (keep_new ? file : g_card_files[same])->path.u8string().c_str());
      if (keep_new) { delete g_card_files[same]; g_card_files[same] = file; } else delete file;
      continue;
    }
    g_card_files[slot++] = file;
  }
  // Imported GCI patches are mounted without copying or modifying the supplied
  // file. Writes create a private copy in the port's card directory; on later
  // mounts that private copy takes precedence over the original import.
  for (const auto& source : g_mod_gcis) {
    if (slot >= CARD_MAX_FILES) host::die("card: no free file slot for imported GCI");
    const auto length = std::filesystem::file_size(source, ec);
    if (ec || length < 64 || length > uint64_t(CARD_TOTAL_BLOCKS) * CARD_SECTOR + 64) {
      host::die("card: invalid imported GCI size: %s", source.string().c_str());
    }
    FILE* in = std::fopen(source.string().c_str(), "rb");
    if (!in) host::die("card: cannot open imported GCI: %s", source.string().c_str());
    auto* file = new SourceCardFile;
    const bool header_ok = std::fread(file->dir, 1, sizeof file->dir, in) == sizeof file->dir;
    const uint32_t blocks = header_ok ? file->blocks() : 0;
    if (!blocks || blocks > CARD_TOTAL_BLOCKS || length != 64ull + uint64_t(blocks) * CARD_SECTOR) {
      std::fclose(in); delete file;
      host::die("card: malformed imported GCI: %s", source.string().c_str());
    }
    file->data.resize(size_t(blocks) * CARD_SECTOR);
    const bool data_ok = std::fread(file->data.data(), 1, file->data.size(), in) == file->data.size();
    std::fclose(in);
    if (!data_ok || !card_same_game(*file)) {
      delete file;
      host::die("card: imported GCI is unreadable or belongs to another game: %s", source.string().c_str());
    }
    const int32_t previous = card_find(file->name());
    const auto private_path = g_card_dir / card_safe_name(*file);
    if (previous >= 0 && g_card_files[previous]->path == private_path) {
      host::log("card: saved private copy supersedes imported %s", source.string().c_str());
      delete file;
      continue;
    }
    if (previous >= 0) {
      const std::string name = file->name();
      delete file;
      host::die("card: imported GCI conflicts with existing card file %s", name.c_str());
    }
    file->path = private_path;
    g_card_files[slot++] = file;
    host::log("card: imported %s as %s (copy-on-write)", source.string().c_str(), file->name().c_str());
  }
  g_card_mounted = true;
  card_refresh_snapshot();
  host::log("card: source slot A mounted from %s (%zu files, %u of %u blocks used)",
            card_shown(g_card_dir).c_str(), slot, card_used_blocks(), CARD_TOTAL_BLOCKS);
}
void card_reset_mount() {
  // Keep the last mounted save's snapshot: Melee unmounts its card after every access, and Friends
  // reads the host's save at START, long after. The next mount, a save, delete, rename or format refreshes it.
  card_clear_files(); g_card_mounted = false;
}
// Seconds since 2000-01-01 on the console clock (its date at start plus game time), as the game's
// own calendar reads it: a save file used to be stamped January 1, 2000.
uint32_t card_time_2000() {

  return host::cpu ? (uint32_t)((host::console_epoch_ticks() + host::cpu->tb) / host::TB_HZ) : 0;
}
void card_fill_stat(const SourceCardFile& file, SourceCardStat& stat) {
  std::memset(&stat, 0, sizeof stat);
  std::memcpy(stat.fileName, file.dir + 8, 32);
  stat.length = (uint32_t)file.data.size();
  stat.time = card_be32(file.dir + 0x28);
  std::memcpy(stat.gameName, file.dir, 4);
  std::memcpy(stat.company, file.dir + 4, 2);
  stat.bannerFormat = file.dir[7];
  stat.iconAddr = card_be32(file.dir + 0x2C);
  stat.iconFormat = card_be16(file.dir + 0x30);
  stat.iconSpeed = card_be16(file.dir + 0x32);
  stat.commentAddr = card_be32(file.dir + 0x3C);
  const uint32_t none = 0xFFFFFFFFu;
  for (uint32_t& offset : stat.offsetIcon) offset = none;
  stat.offsetBanner = stat.offsetBannerTlut = stat.offsetIconTlut = none;
  if (stat.iconAddr == none) { stat.offsetData = 0; return; }
  uint32_t offset = stat.iconAddr;
  if ((stat.bannerFormat & 3) == 1) { stat.offsetBanner = offset; offset += 3072; stat.offsetBannerTlut = offset; offset += 512; }
  else if ((stat.bannerFormat & 3) == 2) { stat.offsetBanner = offset; offset += 6144; }
  bool has_tlut = false;
  for (int i = 0; i < 8; ++i) {
    const uint32_t format = (stat.iconFormat >> (2 * i)) & 3;
    if (format == 1) { stat.offsetIcon[i] = offset; offset += 1024; has_tlut = true; }
    else if (format == 2) { stat.offsetIcon[i] = offset; offset += 2048; }
  }
  if (has_tlut) { stat.offsetIconTlut = offset; offset += 512; }
  stat.offsetData = offset;
}
}  // namespace

// The Lossless adapter's lag test (host/lag_test_codes.h): its app boots Dolphin with no memory card,
// and the "Skip Memcard Prompt" code it relies on skips only that case, so slot A is empty too.
static bool card_slot_missing(int32_t chan) {
  static const bool lag_test = std::getenv("MELEE_LAG_TEST") != nullptr && !std::getenv("MELEE_LAG_TEST_KEEP_CARD");
  return chan != 0 || lag_test;
}


void origin_end_card_session() {
  card_reset_mount();
}












int32_t h_card_probe(int32_t chan, int32_t* mem_size, int32_t* sector_size) {
  if (card_slot_missing(chan)) return CARD_NOCARD;
  if (mem_size) *mem_size = CARD_MEM_SIZE_MBIT;
  if (sector_size) *sector_size = CARD_SECTOR;
  return CARD_READY;
}
int32_t h_card_mount(int32_t chan) { if (card_slot_missing(chan)) return CARD_NOCARD; card_mount_files(); return CARD_READY; }
int32_t h_card_unmount(int32_t chan) { if (chan != 0) return CARD_NOCARD; card_reset_mount(); return CARD_READY; }
int32_t h_card_open(int32_t chan, const char* name, int32_t* file_no, uint32_t* length) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  const int32_t no = card_find(name ? name : "");
  if (no < 0) return CARD_NOFILE;
  if (file_no) *file_no = no;
  if (length) *length = (uint32_t)g_card_files[no]->data.size();
  return CARD_READY;
}
int32_t h_card_close(int32_t chan, int32_t) { return chan == 0 ? CARD_READY : CARD_NOCARD; }
int32_t h_card_create(int32_t chan, const char* name, uint32_t size, int32_t* file_no) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  const std::string filename = name ? name : "";
  if (filename.empty() || filename.size() > 32) return CARD_NAMETOOLONG;
  if (card_find(filename) >= 0) return CARD_EXIST;
  if (size == 0 || size % CARD_SECTOR) return CARD_FATAL;
  const uint32_t blocks = size / CARD_SECTOR;
  if (card_used_blocks() + blocks > CARD_TOTAL_BLOCKS) return CARD_INSSPACE;
  int32_t no = -1;
  for (size_t i = 0; i < g_card_files.size(); ++i) if (!g_card_files[i]) { no = (int32_t)i; break; }
  if (no < 0) return CARD_NOENT;
  auto* file = new SourceCardFile;
  std::memcpy(file->dir, card_disk_id(), 6);
  file->dir[6] = 0xFF; file->dir[7] = 0;
  std::memcpy(file->dir + 8, filename.data(), filename.size());
  card_put32(file->dir + 0x28, card_time_2000());
  card_put32(file->dir + 0x2C, 0xFFFFFFFFu);
  card_put16(file->dir + 0x30, 0); card_put16(file->dir + 0x32, 0);
  file->dir[0x34] = 0x04; file->dir[0x35] = 0;
  card_put16(file->dir + 0x36, (uint16_t)(5 + card_used_blocks()));
  card_put16(file->dir + 0x38, (uint16_t)blocks);
  file->dir[0x3A] = 0xFF; file->dir[0x3B] = 0xFF; card_put32(file->dir + 0x3C, 0xFFFFFFFFu);
  file->data.assign(size, 0xFF);
  g_card_files[no] = file;
  card_save(*file);
  if (file_no) *file_no = no;
  return CARD_READY;
}
int32_t h_card_delete(int32_t chan, const char* name) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  const int32_t no = card_find(name ? name : "");
  if (no < 0) return CARD_NOFILE;
  SourceCardFile* file = g_card_files[no];
  std::error_code ec; std::filesystem::remove(file->path, ec);
  delete file; g_card_files[no] = nullptr;
  card_refresh_snapshot();
  return CARD_READY;
}
int32_t h_card_rename(int32_t chan, const char* old_name, const char* new_name) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  const std::string old_file = old_name ? old_name : "", new_file = new_name ? new_name : "";
  if (new_file.empty() || new_file.size() > 32) return CARD_NAMETOOLONG;
  const int32_t no = card_find(old_file);
  if (no < 0) return CARD_NOFILE;
  if (card_find(new_file) >= 0) return CARD_EXIST;
  SourceCardFile& file = *g_card_files[no];
  std::error_code ec; std::filesystem::remove(file.path, ec); file.path.clear();
  std::memset(file.dir + 8, 0, 32); std::memcpy(file.dir + 8, new_file.data(), new_file.size());
  card_save(file);
  card_refresh_snapshot();
  return CARD_READY;
}
int32_t h_card_read(int32_t chan, int32_t file_no, void* dst, uint32_t length, uint32_t offset) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  if (file_no < 0 || file_no >= (int32_t)g_card_files.size() || !g_card_files[file_no]) return CARD_NOFILE;
  SourceCardFile& file = *g_card_files[file_no];
  if ((uint64_t)offset + length > file.data.size()) return CARD_LIMIT;
  std::memcpy(dst, file.data.data() + offset, length); return CARD_READY;
}
int32_t h_card_write(int32_t chan, int32_t file_no, const void* src, uint32_t length, uint32_t offset) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  if (file_no < 0 || file_no >= (int32_t)g_card_files.size() || !g_card_files[file_no]) return CARD_NOFILE;
  SourceCardFile& file = *g_card_files[file_no];
  if ((uint64_t)offset + length > file.data.size()) return CARD_LIMIT;
  std::memcpy(file.data.data() + offset, src, length); card_save(file); return CARD_READY;
}
int32_t h_card_stat(int32_t chan, int32_t file_no, void* stat, uint32_t stat_size) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  if (file_no < 0 || file_no >= (int32_t)g_card_files.size() || !g_card_files[file_no]) return CARD_NOFILE;
  SourceCardStat value{}; card_fill_stat(*g_card_files[file_no], value);
  std::memcpy(stat, &value, std::min<uint32_t>(stat_size, sizeof value)); return CARD_READY;
}
int32_t h_card_set_stat(int32_t chan, int32_t file_no, const void* stat, uint32_t stat_size) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  if (file_no < 0 || file_no >= (int32_t)g_card_files.size() || !g_card_files[file_no]) return CARD_NOFILE;
  if (stat_size < sizeof(SourceCardStat)) return CARD_LIMIT;
  SourceCardStat value{}; std::memcpy(&value, stat, sizeof value);
  SourceCardFile& file = *g_card_files[file_no];
  file.dir[7] = value.bannerFormat; card_put32(file.dir + 0x2C, value.iconAddr);
  card_put16(file.dir + 0x30, value.iconFormat); card_put16(file.dir + 0x32, value.iconSpeed);
  card_put32(file.dir + 0x3C, value.commentAddr); card_put32(file.dir + 0x28, card_time_2000());
  card_save(file); return CARD_READY;
}
int32_t h_card_free_blocks(int32_t chan, int32_t* bytes_unused, int32_t* files_unused) {
  if (chan != 0 || !g_card_mounted) return CARD_NOCARD;
  if (bytes_unused) *bytes_unused = (int32_t)((CARD_TOTAL_BLOCKS - std::min(CARD_TOTAL_BLOCKS, card_used_blocks())) * CARD_SECTOR);
  if (files_unused) *files_unused = (int32_t)(CARD_MAX_FILES - std::count_if(g_card_files.begin(), g_card_files.end(), [](const SourceCardFile* f) { return f != nullptr; }));
  return CARD_READY;
}
int32_t h_card_format(int32_t chan) {
  if (chan != 0) return CARD_NOCARD;
  card_mount_files();
  for (SourceCardFile*& file : g_card_files) {
    if (!file) continue;
    std::error_code ec; std::filesystem::remove(file->path, ec);
    delete file; file = nullptr;
  }
  card_refresh_snapshot();
  return CARD_READY;
}

bool card_self_test_impl(const char* directory) {
  if (!directory || !*directory) return false;
  host::options.card_dir = directory;
  card_reset_mount();
  const uint8_t id[6] = {'G', 'A', 'L', 'E', '0', '1'};
  std::memcpy(reinterpret_cast<void*>(MEM1_BASE), id, sizeof id);
  int32_t mem_size = 0, sector_size = 0;
  if (h_card_probe(0, &mem_size, &sector_size) != CARD_READY || mem_size != (int32_t)CARD_MEM_SIZE_MBIT ||
      sector_size != (int32_t)CARD_SECTOR || h_card_mount(0) != CARD_READY)
    return false;
  if (h_card_mount(1) != CARD_NOCARD || h_card_create(1, "MU_NATIVE_CARD_TEST", CARD_SECTOR, nullptr) != CARD_NOCARD ||
      h_card_open(0, "missing", nullptr, nullptr) != CARD_NOFILE)
    return false;
  int32_t file_no = -1;
  if (h_card_create(0, "MU_NATIVE_CARD_TEST", CARD_SECTOR, &file_no) != CARD_READY || file_no < 0)
    return false;
  if (h_card_create(0, "MU_NATIVE_CARD_TEST", CARD_SECTOR, nullptr) != CARD_EXIST ||
      h_card_create(0, "MU_NATIVE_CARD_BAD_SIZE", CARD_SECTOR - 1, nullptr) != CARD_FATAL ||
      h_card_read(0, -1, nullptr, 0, 0) != CARD_NOFILE ||
      h_card_write(0, -1, nullptr, 0, 0) != CARD_NOFILE)
    return false;
  std::vector<uint8_t> expected(CARD_SECTOR), actual(CARD_SECTOR);
  for (uint32_t i = 0; i < expected.size(); ++i) expected[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
  if (h_card_read(0, file_no, actual.data(), (uint32_t)actual.size(), CARD_SECTOR) != CARD_LIMIT ||
      h_card_write(0, file_no, expected.data(), (uint32_t)expected.size(), CARD_SECTOR) != CARD_LIMIT)
    return false;
  for (uint32_t pass = 0; pass < 3; ++pass) {
    for (uint32_t i = 0; i < expected.size(); ++i)
      expected[i] = (uint8_t)((i * (37u + pass * 2u) + 11u + pass) & 0xFFu);
    if (h_card_write(0, file_no, expected.data(), (uint32_t)expected.size(), 0) != CARD_READY ||
        h_card_read(0, file_no, actual.data(), (uint32_t)actual.size(), 0) != CARD_READY || actual != expected)
      return false;
  }
  SourceCardStat stat{};
  if (h_card_stat(0, file_no, &stat, sizeof stat) != CARD_READY || stat.length != CARD_SECTOR)
    return false;
  if (h_card_unmount(0) != CARD_READY || h_card_mount(0) != CARD_READY)
    return false;
  int32_t reopened = -1; uint32_t length = 0;
  if (h_card_open(0, "MU_NATIVE_CARD_TEST", &reopened, &length) != CARD_READY ||
      reopened < 0 || length != CARD_SECTOR || h_card_read(0, reopened, actual.data(), (uint32_t)actual.size(), 0) != CARD_READY ||
      actual != expected)
    return false;
  if (h_card_rename(0, "MU_NATIVE_CARD_TEST", "MU_NATIVE_CARD_RENAMED") != CARD_READY ||
      h_card_open(0, "MU_NATIVE_CARD_TEST", nullptr, nullptr) != CARD_NOFILE ||
      h_card_open(0, "MU_NATIVE_CARD_RENAMED", &reopened, &length) != CARD_READY ||
      h_card_read(0, reopened, actual.data(), (uint32_t)actual.size(), 0) != CARD_READY || actual != expected ||
      h_card_delete(0, "MU_NATIVE_CARD_RENAMED") != CARD_READY ||
      h_card_open(0, "MU_NATIVE_CARD_RENAMED", nullptr, nullptr) != CARD_NOFILE)
    return false;
  if (h_card_format(0) != CARD_READY || h_card_unmount(0) != CARD_READY)
    return false;
  host::log("card: native isolated repeated-I/O, bounds, error, rename, delete, and remount checks passed");
  return true;
}

}  // namespace host_internal




bool card_self_test(const char* directory) { return card_self_test_impl(directory); }

}  // namespace source_port
