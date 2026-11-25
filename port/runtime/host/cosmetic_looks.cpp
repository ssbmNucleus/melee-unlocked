// Cosmetic exact looks: the community looks built from the player's retail disc and patch,
// their files' checks, and the one-click official looks.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"
#include <cerrno>

namespace host::cosmetics {

// An exact look's fighter data file (PlXx.dat) with only its parts tables taken from the pack. The
// pack's costumes have their own joints, and its builds point each fighter's parts table (ftData
// +0x08) at them: the costume texture animation joints (ftanim.c, "can't find fighter texture anim"
// when they do not fit) and the parts visibility lists. Everything that table points to, and only
// reaches through it, comes from the pack; the table's own bytes (+0x10 to +0x14 name the parts that
// hold and spawn items) and every other byte (attributes, actions, items) stay the disc's. Both files
// must have the same length, relocations and symbols, and no pointer under those blocks may differ.
// Empty when nothing qualifies or nothing differs.
std::vector<uint8_t> look_fighter_tables(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack) {
  if (clean.size() != pack.size() || clean.size() < 0x20) return {};
  const uint32_t data_size = be32(clean.data() + 4), relocs = be32(clean.data() + 8), roots = be32(clean.data() + 12),
                 refs = be32(clean.data() + 16);
  const uint64_t tables = 0x20ull + data_size;
  if (tables + 4ull * relocs + 8ull * (roots + (uint64_t)refs) > clean.size() ||
      !std::equal(clean.begin() + (ptrdiff_t)tables, clean.end(), pack.begin() + (ptrdiff_t)tables) ||
      !std::equal(clean.begin(), clean.begin() + 0x20, pack.begin()))
    return {};
  const uint8_t* data = clean.data() + 0x20;
  std::map<uint32_t, uint32_t> pointers;   // pointer word offset -> target
  for (uint32_t i = 0; i < relocs; ++i) {
    const uint32_t at = be32(clean.data() + tables + 4ull * i);
    if (at + 4ull > data_size) return {};
    pointers[at] = be32(data + at);
  }
  std::vector<uint32_t> starts{data_size};
  for (const auto& p : pointers) starts.push_back(p.second);
  uint32_t root = UINT32_MAX;
  // The names follow the root and the reference entries (Link's, Young Link's, the Ice Climbers' and
  // Kirby's files have references: without them in the offset no ftData root was found there).
  const uint64_t names = tables + 4ull * relocs + 8ull * (roots + (uint64_t)refs);
  for (uint32_t i = 0; i < roots; ++i) {
    const uint8_t* entry = clean.data() + tables + 4ull * relocs + 8ull * i;
    const uint64_t name = names + be32(entry + 4);
    starts.push_back(be32(entry));
    if (name < clean.size() && std::strncmp((const char*)clean.data() + name, "ftData", 6) == 0) root = be32(entry);
  }
  if (root == UINT32_MAX || root + 0x60ull > data_size) return {};
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  auto block_of = [&](uint32_t offset) {
    auto it = std::upper_bound(starts.begin(), starts.end(), offset);
    const uint32_t begin = it == starts.begin() ? 0 : *(it - 1);
    return std::make_pair(begin, it == starts.end() ? data_size : *it);
  };
  // The blocks each of ftData's 24 fields reaches; a block reached from any field but +0x08 is kept.
  std::map<uint32_t, uint32_t> reached;   // block start -> bit set of the fields that reach it
  for (uint32_t field = 0; field < 24; ++field) {
    const auto slot = pointers.find(root + 4 * field);
    if (slot == pointers.end()) continue;
    std::vector<uint32_t> stack{slot->second};
    while (!stack.empty()) {
      const auto block = block_of(stack.back());
      stack.pop_back();
      if (block.first >= data_size || (reached[block.first] & (1u << field))) continue;
      reached[block.first] |= 1u << field;
      for (auto p = pointers.lower_bound(block.first); p != pointers.end() && p->first < block.second; ++p)
        stack.push_back(p->second);
    }
  }
  const auto parts = pointers.find(root + 8);
  if (parts == pointers.end()) return {};
  const uint32_t parts_table = block_of(parts->second).first;
  std::vector<uint8_t> merged = clean;
  bool changed = false;
  for (const auto& block : reached) {
    if (block.second != (1u << 2) || block.first == parts_table || block.first == block_of(root).first) continue;
    const uint32_t end = block_of(block.first).second;
    for (auto p = pointers.lower_bound(block.first); p != pointers.end() && p->first < end; ++p)
      if (std::memcmp(data + p->first, pack.data() + 0x20 + p->first, 4) != 0) return {};
    for (uint32_t i = block.first; i < end; ++i) {
      if (merged[0x20 + i] == pack[0x20 + i]) continue;
      merged[0x20 + i] = pack[0x20 + i];
      changed = true;
    }
  }
  return changed ? merged : std::vector<uint8_t>{};
}

// A look's file served online and in replays passes the checks the same file passes when it is
// imported: the announcer's names as a sound bank (parse_bank), the pause screen as a pause import
// (pause_screen_valid), the HUD's model joints (no particle or spline joints), and an effect file's
// texture bank in the disc's shape (only its pixels may differ; its programs are compared below).
bool look_file_safe(const std::string& path, const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack,
                           std::string* why) {
  const std::string low = lower(path), base = low.substr(low.find_last_of('/') + 1);
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".ssm") == 0) {
    BankLayout layout;
    return parse_bank(pack, &layout, why);
  }
  if (base == "gmpause.usd") return pause_screen_valid(pack, why);
  data_safety::Archive archive(pack);
  if (!archive.parse()) { *why = "its archive tables do not hold together"; return false; }
  if (base == "ifall.usd") return data_safety::hud_models_plain(archive, why);
  data_safety::Archive original(clean);
  if (!original.parse()) { *why = "the disc's own file cannot be read"; return false; }
  return data_safety::effect_banks_valid(archive, why) && data_safety::effect_textures_same(original, archive, why);
}


// ---- Exact looks: the three community looks built from the player's own retail disc and patch ----

namespace cosmetic_internal {
struct LookPin { const char* name; const char* disc; uint64_t size; const char* sha; };
// The patches as published (version 1.0.2) and the discs they make from the retail NTSC 1.02 disc.
constexpr const char* kLookClassicPatch = "DietMeleeClassic.xdelta";
constexpr uint64_t kLookClassicPatchSize = 186901802;
constexpr const char* kLookClassicPatchSha = "46eb93b7df529fdf7d988acb01483f5aee9e9dae6873e3fd28a282629864ab2e";
struct LookDiff { const char* file; uint64_t size; const char* sha; };
const LookDiff kLookDiffs[] = {
  {"crystaldiff.xdelta", 10666646, "e4256addf1ae46c9c61ca3b648a6e0a1afc7cd8bda30a19203c3bf91a759b76d"},
  {"64diff.xdelta", 13726850, "f8992c8c5efdf66b1a0979ce361263059dc0615921a6df3b7ce7a3e9c6cfe00a"},
};
const LookPin kLookPins[] = {
  {"Diet Classic", "Diet Classic.iso", 287071536, "6164d2e36bbc60e1b8f4ff6665c4eeb0dc43cece1803fe592b6346f2da517f58"},
  {"Crystal", "Crystal.iso", 251487184, "6bb7bd8bc6b824623ab54f1db932a24983286f7b9040a27385d93f7153947bbf"},
  {"Diet 64", "Diet 64.iso", 258289764, "132bc40e3a9281e35d0294c8259f22df48b6ef121edc113f0568a025445fc60f"},
};
constexpr const char* kRetailSha = "0de05981a34156b9cedcef73c73d4244ac05cf6149ab3c9cfed917698819e464";

fs::path looks_dir_locked() { return g_root.parent_path() / L"Looks"; }

// Finds a file by name anywhere under a folder (the patcher's own layout puts them in blob/patches).
fs::path find_named(const fs::path& folder, const std::string& name) {
  std::error_code ec;
  if (fs::is_regular_file(folder / fs::u8path(name), ec)) return folder / fs::u8path(name);
  fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && lower(it->path().filename().u8string()) == lower(name)) return it->path();
  return {};
}

// SHA-256 of a whole file, read in 4 MB pieces (the retail disc is 1.4 GB). Empty on error.
std::string look_file_sha256(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); return {}; }
  std::vector<char> buffer(4u << 20);
  bool ok = true;
  while (ok && file) {
    file.read(buffer.data(), (std::streamsize)buffer.size());
    const std::streamsize got = file.gcount();
    if (got > 0) ok = BCryptHashData(hash, (PUCHAR)buffer.data(), (ULONG)got, 0) >= 0;
  }
  uint8_t digest[32]{};
  ok = ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
  BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) return {};
  std::ostringstream text; text << std::hex << std::setfill('0');
  for (uint8_t byte : digest) text << std::setw(2) << (unsigned)byte;
  return text.str();
}

// Why a disc that plays is still not the exact file the patches were made against: the game reads
// files, the patches need every byte (padding included). Empty when the disc is that file.
std::string retail_disc_problem(const fs::path& path) {
  constexpr uint64_t kRetailSize = 1459978240;
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  char head[8]{};
  errno = 0;
  std::ifstream file(path, std::ios::binary);
  const int open_error = errno;
  errno = 0;
  if (ec || !file || !file.read(head, sizeof head)) {
    const int read_error = errno;
    std::string why;
    if (ec) why = ec.message();
    else if (!file.is_open())
      why = open_error ? std::generic_category().message(open_error) : "the file could not be opened";
    else if (file.eof() && !file.bad()) why = "the disc header is incomplete";
    else why = read_error ? std::generic_category().message(read_error) : "the disc header could not be read";
    host::log("cosmetics: the game disc '%s' could not be read for the looks (%s)",
              path.u8string().c_str(), why.c_str());
    if (path.empty()) return "The game disc file could not be read. This game was started without one.";
    return "The game disc file could not be read (" + path.filename().u8string() + ": " + why
         + "). Check the file and select it again when starting the game.";
  }
  if (std::memcmp(head, "GALE01", 6) != 0) {
    if (size != kRetailSize)
      return "The game disc is a compressed or trimmed copy. These looks need the full 1.36 GB .iso: convert it to a "
             "plain ISO in Dolphin (right-click the game, Convert File, ISO) and choose that file.";
    return "The game disc is not the North American Melee disc these looks are made from.";
  }
  if ((uint8_t)head[7] != 2)
    return "The game disc is NTSC version 1.0" + std::to_string((uint8_t)head[7]) +
           ". These looks are made from version 1.02.";
  if (size != kRetailSize)
    return "The game disc is a trimmed copy of 1.02 (" + std::to_string(size / (1024 * 1024)) +
           " MB, the full disc is 1392 MB). These looks need the untrimmed .iso.";
  if (look_file_sha256(path) == kRetailSha) return {};
  return "The game disc is 1.02 and plays, but its bytes differ from a clean copy: files were changed, or its unused "
         "space was blanked out when it was shrunk or converted. These looks need an untouched dump of the disc.";
}

bool file_matches(const fs::path& file, uint64_t size, const char* sha) {
  std::error_code ec;
  return fs::is_regular_file(file, ec) && fs::file_size(file, ec) == size && !ec && look_file_sha256(file) == sha;
}

#ifdef _WIN32
fs::path prepare_xdelta(const fs::path& preferred, bool recover, std::string* error);
// xdelta3 -d -f -s <source> <patch> <out>, hidden, waited for (up to 10 minutes: the discs are big).
bool run_xdelta(const fs::path& xdelta, const fs::path& source, const fs::path& patch, const fs::path& out, std::string* error) {
  fs::path tool = prepare_xdelta(xdelta, false, error);
  if (tool.empty()) return false;
  STARTUPINFOW startup{}; startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  const auto start = [&] {
    std::wstring command = quote_windows_arg(tool.wstring()) + L" -d -f -s " + quote_windows_arg(source.wstring()) + L" " +
                           quote_windows_arg(patch.wstring()) + L" " + quote_windows_arg(out.wstring());
    return CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &process) != FALSE;
  };
  if (!start()) {
    // A partially extracted install, removed dependency or wrong executable should repair itself
    // in the user's writable catalog, without asking them to find a tool or replace the game.
    tool = prepare_xdelta(xdelta, true, error);
    if (tool.empty()) return false;
    if (!start()) {
      *error = "The repaired patch tool could not start (Windows error " + std::to_string(GetLastError()) + ").";
      return false;
    }
  }
  const DWORD wait = WaitForSingleObject(process.hProcess, 600000);
  if (wait == WAIT_TIMEOUT) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
  DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  const bool loader_failed = code == 0xC0000135u || code == 0xC000007Bu || code == 0xC0000142u;
  if (wait == WAIT_OBJECT_0 && loader_failed) {
    const fs::path repaired = prepare_xdelta(xdelta, true, error);
    if (!repaired.empty() && repaired != tool) return run_xdelta(repaired, source, patch, out, error);
  }
  if (wait != WAIT_OBJECT_0 || code != 0) {
    *error = wait == WAIT_TIMEOUT ? "Building the patch timed out." :
        "The patch tool failed (exit code " + std::to_string(code) + ").";
    return false;
  }
  return true;
}
#endif

}  // namespace cosmetic_internal

std::vector<LookPackStatus> look_packs() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<LookPackStatus> out;
  for (const auto& pin : kLookPins) {
    LookPackStatus status; status.name = pin.name;
    for (const auto& asset : g_assets)
      if (asset.source_kind == kDiscSource && asset.source_name == pin.name) ++status.files;
    status.built = status.files > 0 || g_look_prepared_this_run;
    out.push_back(status);
  }
  return out;
}

// At startup, with the game disc open: every built look disc is scanned as its pack (unchanged discs are
// skipped by scan_disc_skins itself). Discovery checks name and size, not the whole-disc hash.
void scan_look_packs() {
  fs::path dir;
  { std::lock_guard<std::mutex> lock(g_mutex); if (!g_configured) return; dir = looks_dir_locked(); }
  for (const auto& pin : kLookPins) {
    const fs::path disc = dir / fs::u8path(pin.disc);
    std::error_code ec;
    if (!fs::is_regular_file(disc, ec) || fs::file_size(disc, ec) != pin.size) continue;
    std::string error;
    const ImportResult r = scan_disc_skins(disc.u8string(), pin.name, &error);
    if (!r.ok) host::log("looks: %s could not be read (%s)", pin.name, (error.empty() ? r.message : error).c_str());
  }
}

void set_active_look_pack(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_look_pack = name;
}

std::string active_look_pack() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_look_pack;
}

std::atomic<bool> g_tool_recovery{true};
void set_tool_recovery(bool allowed) { g_tool_recovery = allowed; }

bool build_look_packs(const std::string& patcher_folder, const std::string& retail_disc, const std::string& xdelta_tool,
                      std::string* message) {
  std::string local; if (!message) message = &local;
#ifndef _WIN32
  (void)patcher_folder; (void)retail_disc; (void)xdelta_tool;
  *message = "Building the exact looks is Windows only for now."; return false;
#else
  fs::path dir;
  { std::lock_guard<std::mutex> lock(g_mutex); if (!g_configured) { *message = "The skin catalog is not running."; return false; } dir = looks_dir_locked(); }
  const fs::path folder = fs::u8path(patcher_folder), retail = fs::u8path(retail_disc);
  std::error_code ec;
  if (const std::string problem = retail_disc_problem(retail); !problem.empty()) { *message = problem; return false; }
  const fs::path xdelta = prepare_xdelta(fs::u8path(xdelta_tool), false, message);
  if (xdelta.empty()) return false;
  const fs::path classic_patch = find_named(folder, kLookClassicPatch);
  if (classic_patch.empty() || !file_matches(classic_patch, kLookClassicPatchSize, kLookClassicPatchSha)) {
    *message = "That folder does not hold the patch files these looks need (version 1.0.2)."; return false;
  }
  fs::create_directories(dir, ec);
  // Classic first: the other two are its differences.
  const fs::path classic = dir / fs::u8path(kLookPins[0].disc);
  if (!file_matches(classic, kLookPins[0].size, kLookPins[0].sha)) {
    if (!run_xdelta(xdelta, retail, classic_patch, classic, message)) return false;
    if (!file_matches(classic, kLookPins[0].size, kLookPins[0].sha)) { fs::remove(classic, ec); *message = "The built disc did not come out as expected."; return false; }
  }
  for (size_t i = 0; i < 2; ++i) {
    const LookPin& pin = kLookPins[i + 1];
    const fs::path out = dir / fs::u8path(pin.disc);
    if (file_matches(out, pin.size, pin.sha)) continue;
    const fs::path diff = find_named(folder, kLookDiffs[i].file);
    if (diff.empty() || !file_matches(diff, kLookDiffs[i].size, kLookDiffs[i].sha)) { *message = std::string("The patch file ") + kLookDiffs[i].file + " is missing or different."; return false; }
    // As the patcher does: the difference is applied to the Classic patch, and the result to the retail disc.
    const fs::path joined = dir / fs::u8path(std::string(pin.name) + ".xdelta");
    if (!run_xdelta(xdelta, classic_patch, diff, joined, message)) return false;
    const bool ok = run_xdelta(xdelta, retail, joined, out, message);
    fs::remove(joined, ec);
    if (!ok) return false;
    if (!file_matches(out, pin.size, pin.sha)) { fs::remove(out, ec); *message = "The built disc did not come out as expected."; return false; }
  }
  { std::lock_guard<std::mutex> lock(g_mutex); g_look_prepared_this_run = true; }
  *message = "Mod artwork is ready. Choose a mod look above, then restart to apply it.";
  host::log("looks: %s", message->c_str());
  return true;
#endif
}

// ---- One-click mod looks: the authors' own downloads ----
// The owner's call (10-29): Diet Melee and Animelee install from Settings in one click. Each comes
// from its authors' own download (nothing is re-hosted), is checked against the SHA-256 of the
// published file before anything is unpacked, and is built from the player's own disc with the
// shipped patch tool, as the authors' patchers do.

namespace cosmetic_internal {
struct OfficialFile { const char* page; const char* file; uint64_t size; const char* sha; };
// Diet Melee's patcher (linked from diet.melee.tv/download): its Linux archive holds the same three
// patches as the Windows one, in one file. Animelee (animelee.xyz, its pack downloads): Complete
// Edition patch 1.0, and its Dragon Ball Z edition 1.0, built on it. Each Animelee is one xdelta for
// the clean 1.02 disc, applied as the authors' own patcher applies it.
const OfficialFile kDietArchive = {"https://www.mediafire.com/file/v6v5yuqd9le7jav/DietMeleeLinuxPatcher.tar.gz/file",
                                   "DietMeleeLinuxPatcher.tar.gz", 196228815, "3f27e034447aa9cdf93b401d78dc5c8f802cb504d416370e55dfb600b2af0a31"};
const OfficialFile kAnimeleeArchives[2] = {
    {"https://www.mediafire.com/file/2ztqob93nrug8yg/ANIMELEE_-_COMPLETE_EDITION_-_PATCH_1.0.zip/file",
     "Animelee CE 1.0.zip", 31549722, "8ed24c56cbcb46a7030fd56560563b85836cd7f44fc547de23fa5aa7a1621a3c"},
    {"https://www.mediafire.com/file/pr8in3kum9kkvg4/ANIMELEE_-_CE_-_DRAGON_BALL_Z_v1.0.zip/file",
     "Animelee DBZ 1.0.zip", 31109499, "8c5d7c091b3ba261dbd6cc9ebc46f8a6ce3d9ed4add25c3f5b35d9f26765b41c"}};
const LookDiff kAnimeleePatches[2] = {
    {"ANIMELEE - COMPLETE EDITION.xdelta", 13882404, "4a3fde4a2119ce57e7894de99fce640063052f051c2395168c492fcc606d5b76"},
    {"ANIMELEE - CE - DRAGON BALL Z.xdelta", 13432818, "18a63bd7d46eb516ffd7415b26e46abc31df6ad0e8dbf2c07d6c979a76a54d78"}};
const LookPin kAnimeleeDiscs[2] = {
    {"Animelee", "Animelee CE 1.0.iso", 1434723876, "77607e4624f6576f2da35584e71116809db44863ecd768c9d94c0e742da1a94d"},
    {"Animelee x DBZ", "Animelee DBZ 1.0.iso", 1437484160, "ecc4750c121c948bb7bff7818dcc037e12b20f39e42ffc76496d72ea8b648bc3"}};
const char* official_name(int which) { return which == 0 ? "Diet Melee" : which == 1 ? "Animelee" : "Animelee x DBZ"; }

std::mutex g_official_mutex;
int g_official_busy = -1;                // the look being installed, -1 none
float g_official_progress = 0;           // its download, 0 to 1
std::string g_official_message[3];

void official_status(int which, const std::string& text, float progress = -1) {
  std::lock_guard<std::mutex> lock(g_official_mutex);
  g_official_message[which] = text;
  if (progress >= 0) g_official_progress = progress;
}

void official_progress(float progress) {
  std::lock_guard<std::mutex> lock(g_official_mutex);
  g_official_progress = progress;
}

// variant 0 Animelee CE, 1 Animelee x DBZ.
fs::path official_disc(const std::string& mods_folder, int variant) {
  return fs::u8path(mods_folder) / L"Discs" / fs::u8path(kAnimeleeDiscs[variant].disc);
}

#ifdef _WIN32
// One HTTPS GET through the host's one HTTP client (http_client.h), redirects followed but never down
// to HTTP. With `out` the body streams to that file (at most `limit` bytes, the progress shown);
// without it, into `text`. `sha256` (optional): the file is kept only when it has that SHA-256.
// Error::None when it worked; else `error` says why.
host::http::Error https_get(const std::string& url, const fs::path* out, std::string* text, uint64_t limit,
                            std::string* error, const char* sha256 = nullptr) {
  using host::http::Error;
  host::http::Request req;
  req.url = url;
  req.max_bytes = limit;
  req.agent = host::http::Agent::Browser;   // a browser-like prefix (the authors' Mediafire pages), then this program
  req.resolve_ms = 10000;
  req.connect_ms = 10000;
  req.send_ms = 30000;
  req.receive_ms = 120000;
  if (sha256) req.expected_sha256 = sha256;
  req.log_label = "looks";
  if (out) req.progress = [limit](uint64_t got, uint64_t) { official_progress((float)got / (float)limit); return true; };
  host::http::Response r = out ? host::http::fetch_to_file(req, *out) : host::http::fetch(req);
  if (r.ok()) {
    if (text) *text = std::move(r.body);
    return Error::None;
  }
  *error = r.error == Error::Status ? "The download page answered " + std::to_string(r.status) + "."
           : r.error == Error::BadAddress ? "The download address is not valid."
           : r.error == Error::HashMismatch ? "The download did not match its verified release."
           : r.error == Error::Connect || r.error == Error::Tls || r.error == Error::Redirect || !r.bytes ? "The download could not be reached."
           : "The download stopped before it finished.";
  return r.error;
}

// The patch tool the Windows release carries in tools\ (tools/package_release.py pins the same file).
constexpr uint64_t kShippedXdeltaSize = 617182;
constexpr const char* kShippedXdeltaSha256 = "d81f59b2fe5e8589c0ee9782e231c805084f4d23dfade413903a4cad63b4e342";

fs::path prepare_xdelta(const fs::path& preferred, bool recover, std::string* error) {
  std::error_code ec;
  // xdelta 3.1.0's own release, the one recovered below when the shipped tool cannot be used.
  constexpr uint64_t size = 624463;
  constexpr const char* sha = "6855c01cf4a1662ba421e6f95370cf9afa2b3ab6c148473c63efe60d634dfb9a";
  // A tool is run only when it is one of these two files: another program under that name (left
  // there, or put there) is not started.
  if (!recover && !preferred.empty() && fs::is_regular_file(preferred, ec)) {
    if (file_matches(preferred, kShippedXdeltaSize, kShippedXdeltaSha256) || file_matches(preferred, size, sha))
      return preferred;
    host::log("looks: %s is not the patch tool the game ships; the verified one is used instead",
              preferred.filename().u8string().c_str());
  }
  if (!g_tool_recovery.load()) { *error = "The patch tool that comes with the game could not be used."; return {}; }
  fs::path root;
  { std::lock_guard<std::mutex> lock(g_mutex); root = g_root; }
  if (root.empty()) { *error = "The cosmetic catalog is not ready to prepare the patch tool."; return {}; }
  const fs::path cached = root / L"tools" / L"xdelta3.exe";
  if (file_matches(cached, size, sha)) return cached;
  fs::create_directories(cached.parent_path(), ec);
  if (ec) { *error = "The patch-tool cache could not be created."; return {}; }
  const fs::path archive = cached.parent_path() / L"xdelta-download.zip";
  const fs::path part = cached.parent_path() / L"xdelta3-3.1.0-x86_64.exe";
  std::string why;
  constexpr uint64_t archive_size = 217891;
  constexpr const char* archive_sha = "b10e0d1df3e212e5a2d37d5c088d2f72ecec760bc3c26caeca51499c3118b93a";
  const host::http::Error got = https_get("https://github.com/jmacd/xdelta-gpl/releases/download/v3.1.0/xdelta3-3.1.0-x86_64.exe.zip",
                                          &archive, nullptr, archive_size, &why, archive_sha);
  const bool fetched = got == host::http::Error::None || got == host::http::Error::HashMismatch;
  const bool verified = got == host::http::Error::None && file_matches(archive, archive_size, archive_sha);
  const bool unpacked = verified && host::archive::extract(archive, cached.parent_path(), host::archive::kPinnedLimits, &why);
  fs::remove(archive, ec);
  if (!unpacked || !file_matches(part, size, sha)) {
    if (unpacked) fs::remove(part, ec);
    *error = fetched ? "The patch tool download did not match its verified release; nothing was run." :
                      "The patch tool could not be prepared: " + why;
    return {};
  }
  fs::remove(cached, ec); ec.clear();
  fs::rename(part, cached, ec);
  if (ec) { *error = "The verified patch tool could not be saved."; return {}; }
  host::log("looks: recovered the verified patch tool in the cosmetic catalog");
  return cached;
}

// The authors' Mediafire page names the file's current download address; only that address is
// followed, and only on Mediafire's own download hosts. The file must then match its pin exactly.
bool fetch_official(const OfficialFile& f, const fs::path& out, std::string* error) {
  std::string page;
  if (https_get(f.page, nullptr, &page, 4u << 20, error) != host::http::Error::None) return false;
  const size_t at = page.find("href=\"https://download");
  const size_t end = at == std::string::npos ? at : page.find('"', at + 6);
  const std::string link = at == std::string::npos || end == std::string::npos ? std::string() : page.substr(at + 6, end - at - 6);
  const size_t host_end = link.find('/', 8);
  if (link.empty() || host_end == std::string::npos || host_end < 22 || link.compare(host_end - 14, 14, ".mediafire.com") != 0) {
    *error = "The authors' download page has changed; get it from their site for now."; return false;
  }
  const host::http::Error got = https_get(link, &out, nullptr, f.size, error, f.sha);
  if (got == host::http::Error::HashMismatch || (got == host::http::Error::None && !file_matches(out, f.size, f.sha))) {
    *error = "The downloaded file is not the one the authors published; nothing was installed."; return false;
  }
  return got == host::http::Error::None;
}

// Unpacked in process (archive_unpack.h), not by Windows' tar.exe: Wine and Proton have none, and some
// Windows installs cannot start it. Entries leaving the folder are refused; every file used afterwards
// is checked against its pin anyway.
bool unpack_download(const fs::path& archive, const fs::path& dest, std::string* error) {
  std::string why, file;
  if (host::archive::extract(archive, dest, host::archive::kPinnedLimits, &why, &file)) return true;
  host::log("looks: %s not unpacked: %s", archive.filename().u8string().c_str(), why.c_str());
  *error = file.empty() ? "The download is damaged; try again."
                        : "The unpacked files could not be written (" + file + "). Check the free space on that drive.";
  return false;
}

bool install_official(int which, const fs::path& retail, const fs::path& xdelta, const std::string& mods_folder, std::string* message) {
  fs::path dir;
  { std::lock_guard<std::mutex> lock(g_mutex); if (!g_configured) { *message = "The skin catalog is not running."; return false; } dir = looks_dir_locked(); }
  const fs::path work = dir / L".download";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work / L"files", ec);
  if (const std::string problem = retail_disc_problem(retail); !problem.empty()) { *message = problem; return false; }
  const OfficialFile& f = which == 0 ? kDietArchive : kAnimeleeArchives[which - 1];
  const fs::path archive = work / fs::u8path(f.file);
  official_status(which, "Downloading from the authors' page...", 0);
  bool ok = fetch_official(f, archive, message);
  if (ok) { official_status(which, "Unpacking..."); ok = unpack_download(archive, work / L"files", message); }
  if (ok && which == 0) {
    official_status(which, "Building Diet Classic, Diet 64 and Crystal from your disc (a few minutes)...");
    ok = build_look_packs((work / L"files").u8string(), retail.u8string(), xdelta.u8string(), message);
    if (ok) *message = "Installed. Choose Look > Diet Classic, Diet 64 or Crystal, then restart.";
  } else if (ok) {
    const int variant = which - 1;
    official_status(which, std::string("Building ") + official_name(which) + " from your disc...");
    const fs::path patch = find_named(work / L"files", kAnimeleePatches[variant].file);
    const fs::path out = official_disc(mods_folder, variant), part = out.wstring() + L".part";
    fs::create_directories(out.parent_path(), ec);
    if (patch.empty() || !file_matches(patch, kAnimeleePatches[variant].size, kAnimeleePatches[variant].sha)) {
      *message = std::string("The ") + official_name(which) + " patch inside the download is missing or different."; ok = false;
    }
    else if (!run_xdelta(xdelta, retail, patch, part, message)) ok = false;
    else if (!file_matches(part, kAnimeleeDiscs[variant].size, kAnimeleeDiscs[variant].sha)) { fs::remove(part, ec); *message = "The built disc did not come out as expected."; ok = false; }
    else {
      ok = host::archive::place_file(part, out, ec);
      *message = ok ? "Installed. It is turned on in Game mods; restart to play with it."
                    : host::archive::place_error("The built disc could not be put", out.parent_path(), ec);
    }
  }
  fs::remove_all(work, ec);
  return ok;
}
#endif

}  // namespace cosmetic_internal

bool official_cosmetic_disc(const std::string& disc_path) {
  // Hashing the 1.4 GB disc took seconds at every start (long enough that an online test peer gave up
  // waiting). Once it has matched, a note beside it records its size and write time, and the same file
  // is trusted from then on; any change to it is hashed again.
  const fs::path disc = fs::u8path(disc_path);
  std::error_code ec;
  const uint64_t size = fs::file_size(disc, ec);
  if (ec) return false;
  const auto written = fs::last_write_time(disc, ec);
  if (ec) return false;
  const std::string stamp = std::to_string(size) + " " +
                            std::to_string((long long)written.time_since_epoch().count());
  const fs::path note = disc.wstring() + L".verified";
  for (const auto& pin : kAnimeleeDiscs) {
    if (size != pin.size) continue;
    {
      std::ifstream in(note);
      std::string line;
      if (in && std::getline(in, line) && line == stamp + " " + pin.sha) return true;
    }
    if (file_matches(disc, pin.size, pin.sha)) {
      std::ofstream(note, std::ios::trunc) << stamp << " " << pin.sha << "\n";
      return true;
    }
  }
  return false;
}

OfficialLookState official_look_state(int which, const std::string& mods_folder) {
  OfficialLookState s;
  if (which < 0 || which > 2) return s;
  if (which == 0) {
    s.installed = true;
    for (const auto& pack : look_packs()) s.installed &= pack.built;
  } else if (which <= 2) {
    std::error_code ec;
    const fs::path disc = official_disc(mods_folder, which - 1);
    s.installed = fs::is_regular_file(disc, ec) && fs::file_size(disc, ec) == kAnimeleeDiscs[which - 1].size;
    s.disc = disc.u8string();
  }
  std::lock_guard<std::mutex> lock(g_official_mutex);
  s.busy = g_official_busy == which;
  s.any_busy = g_official_busy >= 0;
  s.progress = g_official_progress;
  s.message = g_official_message[which];
  return s;
}

void install_official_look(int which, const std::string& retail_disc, const std::string& xdelta_tool, const std::string& mods_folder) {
  if (which < 0 || which > 2) return;
  {
    std::lock_guard<std::mutex> lock(g_official_mutex);
    if (g_official_busy >= 0) return;
    g_official_busy = which; g_official_progress = 0; g_official_message[which].clear();
  }
#ifndef _WIN32
  (void)retail_disc; (void)xdelta_tool; (void)mods_folder;
  official_status(which, "Installing from Settings is Windows only for now.");
  std::lock_guard<std::mutex> lock(g_official_mutex); g_official_busy = -1;
#else
  std::thread([which, retail_disc, xdelta_tool, mods_folder] {
    std::string message;
    const bool ok = install_official(which, fs::u8path(retail_disc), fs::u8path(xdelta_tool), mods_folder, &message);
    host::log("looks: %s install %s: %s", official_name(which), ok ? "done" : "failed", message.c_str());
    official_status(which, message, ok ? 1.0f : -1.0f);
    std::lock_guard<std::mutex> lock(g_official_mutex);
    g_official_busy = -1;
  }).detach();
#endif
}

}  // namespace host::cosmetics
