// The cosmetic catalog's files share this: the includes, the catalog's types and constants, its state
// (extern) and the helpers more than one file uses, each under the file that defines it.
// cosmetic_mods.h is the module's interface; nothing outside cosmetic_*.cpp includes this.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define NOMINMAX
#include "test_env.h"
#include "cosmetic_mods.h"
#include "dat_check.h"
#include "costume_skeleton.h"
#include "disc_costume_metadata.h"
#include "gameplay_policy.h"
#include "mod_data_safety.h"
#include "stage_dat_safety.h"
#include "stage_read_pin.h"
#include "stage_graft.h"

#include "host.h"
#include "archive_unpack.h"
#include "http_client.h"   // one-click mod looks (install_official_look)

#include <windows.h>
#include <bcrypt.h>
#include <thread>

#include <algorithm>
#include <functional>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "nlohmann/json.hpp"

namespace host::cosmetics {
namespace cosmetic_internal {
using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr uint32_t kCatalogSchema = 1;
constexpr uint32_t kProfileSchema = 1;
constexpr uint32_t kStateSchema = 1;
constexpr uint32_t kMaxDatCandidates = 32;
inline constexpr const char* kVanillaSelection = "@vanilla";

struct AssetRecord {
  struct Companion {
    std::string kind;
    std::string stored_path;
    std::string sha256;
    std::string source_member;
  };
  AssetInfo info;
  std::string stored_path;
  std::string source_kind;
  std::string source_name;
  std::string source_member;
  std::string source_id;
  std::vector<Companion> companions;
  std::map<std::string, std::string> costume_aliases;  // m-ex exports -> native slot exports, applied after source hash verification
  // A skin that lives inside a mod disc or a files pack (source_kind "disc"): no copy is kept. The
  // bytes are read from disc_path at disc_offset when the skin is applied, and checked against the
  // catalog's SHA-256 then. disc_size and disc_mtime are the container's size and time at the scan,
  // for a cheap "is it still the same file" check at startup. source_id is the scanned disc or folder.
  std::string disc_path;
  uint64_t disc_offset = 0, disc_length = 0, disc_size = 0;
  int64_t disc_mtime = 0;
  // The skeleton verdict taken when the scan had both files in hand, so the list can say "stays on
  // online" before the skin was ever applied. Imports have none until they are applied.
  bool online_known = false, online_ok = false;
  std::string online_note;
  // The fighter sound bank this skin brings (docs/voice-mods.md): a file of the catalog, checked
  // against its SHA-256 when it is applied. `bank` is the disc file it replaces ("fox.ssm"). Empty
  // stored_path: the skin has none.
  struct Voice { std::string stored_path, sha256, source_member, bank; } voice;
};

// One line per scanned disc: scanning reads every costume file, so it runs once per disc and again
// only when the file's size or time changes, or when the scan itself learned something new (rules).
struct DiscScan { std::string path; uint64_t size = 0; int64_t mtime = 0; uint32_t rules = 0; };
// 1: .dat and .usd costumes. 2: alternate costumes under other extensions (.lat, .rat) and the online
// verdict taken at scan time. 3: the pack's own portraits, from its character select file. 4: the
// skeleton check reads the relocation table (a joint at data offset 0 is a joint) and its reason
// names the bone. 5: a costume whose blended mesh names a bone with no bind matrix is marked, since
// the game stops when it draws one. 6: stage skins. 7: fighter move sound archives. A disc scanned
// under an older number is scanned again at the next boot. 9: m-ex costume filenames and the
// portrait animation's own stride, dimensions and palette keys, including added costumes. 10: an m-ex
// stage stub (stage code, no collision data) is not a stage skin, and the alternates of a
// dynamic alternate stages disc ("GrSt/GrSt-name(L).dat") are stage skins of their stage. 11: an m-ex
// costume's stock icon from the HUD file's "Stc_icns".
constexpr uint32_t kScanRules = 11;
inline constexpr const char* kImportPack = "import";
inline constexpr const char* kDiscSource = "disc";
inline constexpr const char* kDiscChanged = "disc file missing or changed";

struct Profile {
  bool enabled = true;
  bool random_stage_skins = false;
  std::map<std::string, bool> stage_packs_off;   // packs whose stage skins stay out of the random pick
  std::map<std::string, bool> pool_vanilla_off;  // stage pools without the standard stage, by stage file
  std::map<std::string, std::vector<std::string>> stadium_sets;   // a Stadium skin -> its GrPs1-4 skins
  uint64_t generation = 1;
  std::map<std::string, std::string> selections;
  // An effect file the slot table covers (cosmetic_effect_slots.cpp): slot name ("Falcon Punch", or
  // effect_slots::kAllSlots for an effect served whole) -> the effect it comes from. A slot not named is
  // the disc's own. Its selection ("EfCaData.dat#effects") names one of these effects while any is
  // named, else it is vanilla. A file with no entry here and its selection set (a profile from before
  // slots): the selected effect is the source of the slots it changes.
  std::map<std::string, std::map<std::string, std::string>> effect_slots;
};

struct RuntimeAsset {
  std::string id;
  std::string target_path;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  uint32_t vanilla_size = 0;
  bool online_allowed = true;
  std::string kind;            // the catalog kind, for the counts the notice shows
  std::string online_reason;   // short text for the Mods tab; empty when there is no online rule (effects)
  // A voice bank file only (kind kVoiceKind): the length the file table carries for it. `bytes` is
  // the chosen skin's bank padded to that length, or null while the disc's own bank is served.
  uint32_t length = 0;
  // What is served while online play or a replay is on, when it differs from `bytes`, at the same
  // length: a reshaped costume skin with the standard rest pose copied in (costume_fix_rest_pose), or
  // under kStrictOnlineStages a stage skin's meshes drawn over the original stage (stage_graft.h).
  // Offline the skin itself is served. Null for everything else.
  std::shared_ptr<const std::vector<uint8_t>> online_bytes;
};

inline constexpr const char* kVoiceKind = "voice_bank";
// A standard stage file served padded to the length its random skins share (publish_locked). Not a
// skin: no id, nothing counted, the same stage online and offline.
inline constexpr const char* kStageLengthKind = "stage_length";
inline constexpr const char* kLookFighterKind = "look_fighter_tables";   // an exact look's fighter data (look_fighter_tables)
inline constexpr const char* kLookFileKind = "look_file";   // an exact look's HUD, pause, announcer or effect file
// The online rule a saved stage verdict was made under. 2 (0.9.0): a skin's gameplay values are put
// back to the original's (stage_safety::restore_gameplay) before it is judged. 3 (0.9.63): a joint's
// behavior flags are put back too (exporters set or clear CLASSICAL_SCALE on every joint). 5: the
// desktop app's rule (costume_fix_rest_pose; stages and effects as installed). 6 (2026-11-25): a costume
// also by the peer rule's other comparisons (costume_online_fix: flags, constraints, class names, roots).
constexpr int kOnlineRule = 6;

// Online, the player's own cosmetics follow the desktop app (owner, 2026-11-24): a costume skin is
// used with the standard rest pose copied in, a stage skin and an effect file are used as installed,
// and only what the game could not load or draw is refused. A costume skin also passes everything else
// an opponent's machine compares (costume_online_fix, 2026-11-25), so a skin plays online on the standard
// costume's bones on both machines or on neither. The client's older,
// stricter rules stay here, each switched off, so one can be put back if a real problem shows up:
// kStrictOnlineStages: a stage skin online only in a form whose gameplay matches the original
//   (stage_online_form: tables restored, else its meshes drawn over the original stage); pool members
//   without one stay out of the pick online.
// kStrictOnlineEffects: an effect file online only when it changes textures alone (or Fox's and
//   Falco's particle colors); otherwise the disc's file.
// kFreezeSkinPicksOnline: no skin or stage skin may be picked from a search until the opponent is gone
//   (the select screen between games of a set included).
constexpr bool kStrictOnlineStages = false;
constexpr bool kStrictOnlineEffects = false;
constexpr bool kFreezeSkinPicksOnline = false;

// One disc bank file ("audio/us/fox.ssm") that at least one installed skin has a fitting bank for.
// The candidates are fixed at startup; plan_match_voices picks among them.
struct VoiceFile {
  uint32_t start = 0, disc_size = 0, length = 0;
  std::string bank, path;   // "fox.ssm", and the disc path for the log
  int bank_index = -1;      // the game's bank number
  std::string chosen;       // the skin serving it now, "" the disc's own bank
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> candidates;   // skin id -> served bytes
};

struct RuntimeState {
  bool initialized = false;
  uint64_t generation = 0;
  std::string fingerprint;
  std::unordered_map<uint32_t, RuntimeAsset> by_start;
  uint32_t assets = 0;   // selected assets applied (an asset with an English twin has two files in by_start)
  std::vector<CompanionOverride> companions;
  std::vector<VoiceFile> voice_files;
  std::map<std::string, std::pair<uint64_t, std::string>> stage_matches;
};

// Zips are read by the host's one zip reader (archive_unpack.h), in process.
using ZipEntry = host::archive::Entry;
using host::archive::Zip;

// ---- stage pools (cosmetic_mods.h; the pick is in cosmetic_stage_pools.cpp) ----
// The six legal stages in the stage select's order: each one's installed skins and the standard stage are
// a pool, one of them picked at random for every match on it.
struct PoolStage { const char* target; const char* name; };
inline constexpr PoolStage kPoolStages[] = {
    {"GrSt.dat", "Yoshi's Story"}, {"GrIz.dat", "Fountain of Dreams"}, {"GrPs.dat", "Pokemon Stadium"},
    {"GrNBa.dat", "Battlefield"},  {"GrNLa.dat", "Final Destination"}, {"GrOp.dat", "Dream Land"}};
// Pokemon Stadium's transformations (grpstadium.c: fire, grass, water, rock): a Stadium skin may bring all
// four, served with it.
inline constexpr const char* kStadiumForms[] = {"GrPs1.dat", "GrPs2.dat", "GrPs3.dat", "GrPs4.dat"};

struct FighterFamily {
  const char* file_code;
  const char* root_name;
  const char* display_name;
  const char* colors;
};
// These are the existing NTSC 1.02 costume files only. Defaults use an un-suffixed 5K root even
// though their disc filename ends in Nr. Bosses, wireframes, Sandbag, extra CSS slots, and base
// fighter data archives are intentionally absent. Colors are in the game's costume order (the
// order the character select screen cycles through), which is not the order of the file names.
inline constexpr FighterFamily families[] = {
    {"Ca", "Captain", "Captain Falcon", "Nr Gy Re Wh Gr Bu"},
    {"Cl", "Clink", "Young Link", "Nr Re Bu Wh Bk"},
    {"Dk", "Donkey", "Donkey Kong", "Nr Bk Re Bu Gr"},
    {"Dr", "Drmario", "Dr. Mario", "Nr Re Bu Gr Bk"},
    {"Fc", "Falco", "Falco", "Nr Re Bu Gr"},
    {"Fe", "Emblem", "Roy", "Nr Re Bu Gr Ye"},
    {"Fx", "Fox", "Fox", "Nr Or La Gr"},
    {"Gn", "Ganon", "Ganondorf", "Nr Re Bu Gr La"},
    {"Gw", "Gamewatch", "Mr. Game & Watch", "Nr"},
    {"Kb", "Kirby", "Kirby", "Nr Ye Bu Re Gr Wh"},
    {"Kp", "Koopa", "Bowser", "Nr Re Bu Bk"},
    {"Lg", "Luigi", "Luigi", "Nr Wh Aq Pi"},
    {"Lk", "Link", "Link", "Nr Re Bu Bk Wh"},
    {"Mr", "Mario", "Mario", "Nr Ye Bk Bu Gr"},
    {"Ms", "Mars", "Marth", "Nr Re Gr Bk Wh"},
    {"Mt", "Mewtwo", "Mewtwo", "Nr Re Bu Gr"},
    {"Nn", "Nana", "Ice Climbers (Nana)", "Nr Ye Aq Wh"},
    {"Ns", "Ness", "Ness", "Nr Ye Bu Gr"},
    {"Pc", "Pichu", "Pichu", "Nr Re Bu Gr"},
    {"Pe", "Peach", "Peach", "Nr Ye Wh Bu Gr"},
    {"Pk", "Pikachu", "Pikachu", "Nr Re Bu Gr"},
    {"Pp", "Popo", "Ice Climbers (Popo)", "Nr Gr Or Re"},
    {"Pr", "Purin", "Jigglypuff", "Nr Re Bu Gr Ye"},
    {"Sk", "Seak", "Sheik", "Nr Re Bu Gr Wh"},
    {"Ss", "Samus", "Samus", "Nr Pi Bk Gr La"},
    {"Ys", "Yoshi", "Yoshi", "Nr Re Bu Ye Pi Aq"},
    {"Zd", "Zelda", "Zelda", "Nr Re Bu Gr Wh"},
};
struct ColorName { const char* code; const char* label; };
inline constexpr ColorName color_names[] = {
    {"Nr", "Default"}, {"Re", "Red"}, {"Bu", "Blue"}, {"Gr", "Green"},
    {"Wh", "White"}, {"Bk", "Black"}, {"Ye", "Yellow"}, {"Or", "Orange"},
    {"La", "Lavender"}, {"Pi", "Pink"}, {"Aq", "Aqua"}, {"Gy", "Gray"},
};

// ---- portraits without a costume file ----
// A portrait (the character select picture) or stock icon the player gives a costume slot on its
// own. It is a catalog entry of its own kind whose target is the slot plus "#portrait": it is
// chosen independently of the slot's skin, and a build that does not know the kind finds no disc
// file by that name and leaves it alone.
inline constexpr const char* kPortraitKind = "character_portrait";
inline constexpr const char* kPortraitSuffix = "#portrait";

struct SlotName { const FighterFamily* family = nullptr; const ColorName* color = nullptr; };
struct BankIdentity { uint32_t count, base; const char* stereo; };
struct VoiceBank { const char* file; int index; const char* fighters; uint32_t budget; BankIdentity language[2]; };
inline constexpr const char* kBankLanguages[2] = {"English", "Japanese"};

struct BankLayout {
  uint32_t table_size = 0, data_size = 0, count = 0, base = 0, data_offset = 0;
  std::vector<uint8_t> channels;   // per sound: 1 or 2
};

struct VisualImage {
  uint32_t target = 0, size = 0;
  uint16_t width = 0, height = 0;
  uint32_t format = 0, mipmap = 0, min_lod_bits = 0, max_lod_bits = 0;
  bool operator==(const VisualImage& other) const {
    return target == other.target && size == other.size && width == other.width &&
           height == other.height && format == other.format && mipmap == other.mipmap &&
           min_lod_bits == other.min_lod_bits && max_lod_bits == other.max_lod_bits;
  }
};
struct VisualPalette {
  uint32_t target = 0, size = 0, format = 0, name = 0;
  uint16_t entries = 0;
  bool operator==(const VisualPalette& other) const {
    return target == other.target && size == other.size && format == other.format &&
           name == other.name && entries == other.entries;
  }
};
struct VisualLayout {
  uint32_t data_size = 0, relocation_start = 0;
  std::vector<uint32_t> relocations;
  std::vector<std::string> roots;
  std::map<uint32_t, VisualImage> images;
  std::map<uint32_t, VisualPalette> palettes;
};

struct VaultCompanionPlan {
  std::string kind;
  std::string source_member;
  std::vector<uint8_t> bytes;
};
struct VaultVariantPlan {
  std::string source_id;
  std::string display_name;
  std::string archive_member;
  std::string dat_member;
  testing::DatInspection dat;
  std::vector<uint8_t> bytes;
  std::vector<VaultCompanionPlan> companions;
  std::vector<std::string> companion_notices;
  std::vector<std::string> dependencies;
};
struct VaultResourcePlan {
  std::string source_id;
  std::string display_name;
  std::string kind;         // stage_visual or effect_visual
  std::string target_path;  // existing clean ISO DAT
  std::string group;        // stage name or character/global group
  std::string slot;         // UI slot label
  std::string source_member;
  std::vector<uint8_t> bytes;
  std::vector<VaultCompanionPlan> companions;
};
struct VaultPlan {
  std::vector<VaultVariantPlan> variants;
  std::vector<VaultResourcePlan> resources;
  std::vector<std::string> rejected_resources;
  size_t stage_records = 0;
  size_t effect_records = 0;
};
// A sound bank checked for one costume slot before anything is written: it parses, it is one of the
// game's fighter banks, and that bank is the fighter's of `slot_target`.
struct VoiceCheck { const VoiceBank* bank = nullptr; int language = 0; BankLayout layout; };

struct FstFile { uint32_t index = 0, start = 0, size = 0; std::string path; };
struct DiscCandidate { std::string member; fs::path file; uint64_t offset = 0, length = 0; };

// ---- the catalog's state (defined in cosmetic_mods.cpp, in the order the one file had it) ----
extern std::mutex g_mutex;
extern fs::path g_root;
extern std::vector<AssetRecord> g_assets;
extern std::vector<DiscScan> g_disc_scans;
extern Profile g_profile;
extern bool g_configured;
extern bool g_catalog_valid;
extern bool g_profile_valid;
extern std::string g_message;
extern std::shared_ptr<const RuntimeState> g_runtime;
extern std::atomic<uint32_t> g_online_freezes;
extern std::string g_session_pack;
extern std::string g_look_pack;
extern std::string g_session_stage_pack;
extern uint32_t (*g_stage_buttons)();
extern bool (*g_lockstep)();
extern bool g_look_prepared_this_run;
extern std::map<std::string, size_t> g_stage_extent;

// ---- cosmetic_mods.cpp ----
uint16_t le16(const uint8_t* p);
uint32_t le32(const uint8_t* p);
uint32_t be32(const uint8_t* p);
uint16_t be16(const uint8_t* p);
void put_be32(uint8_t* p, uint32_t value);
std::string lower(std::string value);
bool dat_extension(const std::string& name);
std::string path_filename_utf8(const fs::path& path);
bool safe_relative_path(const std::string& raw);
bool read_bounded(const fs::path& path, uint64_t limit, std::vector<uint8_t>* out,
                  std::string* error);
bool file_stamp(const fs::path& path, uint64_t* size, int64_t* mtime);
bool read_range(const fs::path& path, uint64_t offset, uint64_t length, std::vector<uint8_t>* out);
uint32_t crc32(const uint8_t* data, size_t size);
std::string sha256(const uint8_t* data, size_t size);
std::string sha256(const std::vector<uint8_t>& data);
std::string sha256_text(const std::string& text);
bool write_atomic(const fs::path& path, const uint8_t* data, size_t size, std::string* error);
bool write_atomic(const fs::path& path, const std::string& text, std::string* error);
json string_array(const std::vector<std::string>& values);
std::vector<std::string> get_string_array(const json& object, const char* key);
bool open_zip(const fs::path& path, Zip* zip, std::string* error);
std::wstring quote_windows_arg(const std::wstring& arg);
bool family_has_color(const FighterFamily& family, const char* code);
std::string slot_file(const SlotName& slot);
bool find_slot(const std::string& target_path, SlotName* out);

// ---- cosmetic_profile.cpp ----
std::string pack_key(const AssetRecord& asset);
bool save_state_locked(std::string* error);
bool save_catalog_locked(std::string* error);
bool save_profile_locked(std::string* error);
std::string desired_fingerprint_locked();
bool ready_locked(std::string* error);
bool mutable_profile_locked(std::string* error);
std::string selection_key(const AssetRecord& asset);
std::vector<uint8_t> load_runtime_asset_locked(const AssetRecord& asset, std::string* error);
bool refresh_assets_locked(bool prune_invalid_selections, std::string* error);

// ---- cosmetic_sounds.cpp ----
bool parse_bank(const std::vector<uint8_t>& bytes, BankLayout* out, std::string* error);
ImportResult install_fighter_sound_locked(const fs::path& source, const std::string& member,
                                          const std::vector<uint8_t>& mod_bytes);
bool check_voice(const std::vector<uint8_t>& ssm, const std::string& source_name, const std::string& slot_target,
                 VoiceCheck* out, std::string* error);
ImportResult attach_voice_locked(const std::string& asset_id, const std::vector<uint8_t>& ssm,
                                 const std::string& source_name);
ImportResult attach_loose_voice_locked(const std::vector<uint8_t>& ssm, const std::string& source_name,
                                       std::string slot_hint, const std::vector<std::string>& names);

// ---- cosmetic_portraits.cpp ----
std::string portrait_slot(const std::string& target);
bool stock_picture_name(const std::string& raw);
bool portrait_slot_from_name_impl(const std::string& raw, std::string* slot, std::string* kind);
ImportResult install_portrait_locked(const std::string& slot_target, const std::string& kind,
                                     const std::vector<uint8_t>& png, const std::string& source_name);
size_t pack_portraits_locked(const DiscCandidate& screen, const disc_metadata::MexCostumes& costumes,
                            std::map<std::string, AssetRecord>* fresh);
size_t pack_stock_icons_locked(const DiscCandidate& hud, const disc_metadata::MexCostumes& costumes,
                               std::map<std::string, AssetRecord>* fresh);

// ---- cosmetic_checks.cpp ----
testing::DatInspection inspect_dat_impl(const std::vector<uint8_t>& bytes);
uint32_t gx_image_size(uint32_t width, uint32_t height, uint32_t format,
                       uint32_t mipmap, float max_lod);
bool parse_visual_layout(const std::vector<uint8_t>& bytes, VisualLayout* out,
                         std::string* error);
// The desktop app's fix (the standard rest pose on every joint; offline, a skin within the old
// tolerance takes it), and the same with everything else the peer rule compares (online: what a disc
// slot's costume skin plays by). cosmetic_checks.cpp.
bool costume_fix_rest_pose(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& skin,
                           std::vector<uint8_t>* fixed, std::string* detail);
bool costume_online_fix(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& skin,
                        std::vector<uint8_t>* fixed, std::string* detail);
bool visual_dat_only(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                     std::string* error);
bool pause_screen_valid(const std::vector<uint8_t>& bytes, std::string* error);
bool pause_filename(const std::string& name);
bool pause_resource(const fs::path& source, const std::string& member, std::vector<uint8_t> bytes,
                    VaultResourcePlan* out, std::string* error);
bool costume_structure_safe(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>* standard, std::string* defect);
const std::vector<uint8_t>* standard_costume(const std::string& target, std::vector<uint8_t>* storage);

// ---- cosmetic_effects.cpp ----
bool effect_archive_target(const std::string& lowered);
bool fighter_base_target(const std::string& lowered);
bool materialize_effect_dat_impl(const std::string& target_path,
                                 const std::vector<uint8_t>& clean,
                                 const std::vector<uint8_t>& candidate,
                                 std::vector<uint8_t>* runtime,
                                 std::string* classification,
                                 std::string* error);
std::string effect_scope(const AssetRecord& asset);
std::string effect_label(const std::string& scope);
bool effect_target_from_roots(const std::vector<std::string>& roots, std::string* path);
bool effect_target(const std::string& character, const std::string& scope,
                   const std::string& model_file,
                   std::string* path, std::string* group, std::string* slot);
bool materialize_effect_with_open_disc(const std::string& target_path,
                                       const std::vector<uint8_t>& candidate,
                                       std::vector<uint8_t>* runtime,
                                       std::string* classification,
                                       std::string* error);
std::vector<const AssetRecord*> selected_effects_locked(const Profile& profile,
                                                         const std::string& target);
bool compose_effects_locked(const std::string& target,
                            const std::vector<const AssetRecord*>& choices,
                            const std::vector<uint8_t>& clean,
                            std::vector<uint8_t>* combined, std::string* error);
bool compose_effects_with_open_disc_locked(const std::string& target,
                                          const Profile& profile, std::string* error);
// The game reads this disc file again for every match, so the file published again while it runs
// (republish_effect) reaches the next match with no restart.
bool effect_file_live(const std::string& lowered);

// ---- cosmetic_effect_slots.cpp ----
bool standalone_effect(const AssetRecord& asset);
// The file served for `target` (an effect file): its slots from their sources over `clean`, then any
// older scoped effects merged on top (compose_effects_locked). `effects` gets how many effects it took.
bool compose_effect_file_locked(const Profile& profile, const std::string& target, const std::vector<uint8_t>& clean,
                                std::vector<uint8_t>* out, size_t* effects, std::string* error);
// `asset` (a standalone effect) the source of the slots it changes; false when its file has no slot
// table (the caller selects it as one choice per file).
bool assign_effect_locked(Profile* profile, const AssetRecord& asset, std::string* error);
// An effect leaving: the slots it was the source of go to the newest other installed effect that
// changes them, else the disc's own. True when the served file changes.
bool release_effect_locked(Profile* profile, const AssetRecord& asset);
// Effect slots naming effects no longer installed are dropped; each file's selection follows its slots.
void prune_effect_slots_locked(Profile* profile);
void forget_effect_slots_locked(Profile* profile, const std::string& target);
// An effect file not installed yet (its disc file and bytes), as effect_takes for an installed one: the
// slots it changes, `whole` when served whole, and what using it would take from the installed effects.
// False when the disc's file cannot be read.
bool effect_would_take_locked(const std::string& target, const std::vector<uint8_t>& bytes, const std::string& digest,
                              std::vector<std::string>* slots, bool* whole, std::vector<EffectTaken>* taken);
void load_effect_slots(const json& profile, Profile* out);
json effect_slots_json(const Profile& profile);

// ---- cosmetic_vault.cpp ----
std::string companion_label(const ZipEntry& entry);
std::string display_name(const fs::path& source, const std::string& member);
bool png_companion(const std::vector<uint8_t>& bytes, std::string* error);
std::string player_picture_file(const std::string& kind, const std::string& digest);
bool player_picture(const AssetRecord::Companion& companion);
bool parse_nucleus_vault(const Zip& zip, const std::vector<ZipEntry>& entries,
                         const ZipEntry& metadata_entry, VaultPlan* plan, std::string* error);
// `select_effects` false: a standalone effect is installed and chosen for nothing (use_effect).
ImportResult install_vault_locked(const fs::path& source, VaultPlan plan,
                                  const std::string& resource_source_kind = "nucleus_project",
                                  bool select_effects = true);

// ---- cosmetic_disc_scan.cpp ----
bool parse_fst(uint8_t* fst, uint32_t fst_size, std::vector<FstFile>* files, std::string* error);
bool disc_skin_set(const std::string& extension, std::string* variant);

// ---- cosmetic_stage_skins.cpp ----
bool stage_target(const std::string& id, std::string* path, std::string* display);
bool stage_dat_target(const std::string& filename, std::string* path, std::string* display);
std::string stadium_form_file(const std::string& filename);
bool stage_structure_safe(const std::vector<uint8_t>& bytes, std::string* defect);
bool disc_stage_member(const std::string& raw, std::string* target, std::string* label, std::string* variant);
uint32_t stage_alternate_button(const std::string& variant);

// ---- cosmetic_stage_pools.cpp ----
const PoolStage* pool_stage_of(const std::string& key);
bool pool_has_skins_locked(const std::string& target);
bool any_pool_locked();
bool pool_owned_key_locked(const std::string& key);
bool look_owns_locked(const std::string& target);

// ---- cosmetic_looks.cpp ----
#ifdef _WIN32
bool run_xdelta(const fs::path& xdelta, const fs::path& source, const fs::path& patch, const fs::path& out, std::string* error);
#endif

}  // namespace cosmetic_internal

using namespace cosmetic_internal;   // as the one file's unnamed namespace was: its names are the catalog's own

// ---- shared by the files, in host::cosmetics itself as they were (not in cosmetic_mods.h) ----
// cosmetic_publish.cpp
extern bool g_live_in_step, g_live_unpublished;
bool session_stage_record(const AssetRecord& asset);
void publish_locked(uint8_t* fst, uint32_t fst_size, const std::string* only_slot, RepublishResult* republished,
                           const std::string* stage_pick = nullptr);

// ---- cosmetic_sounds.cpp ----
void publish_voices_locked(uint8_t* fst, const std::vector<FstFile>& files, RuntimeState* next);
bool fighter_sound_identity(const std::string& raw, size_t* kind, const char** character);
bool fighter_sound_overlay(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& mod,
                                  size_t fighter_kind, std::vector<uint8_t>* merged, std::string* error);

// ---- cosmetic_effects.cpp ----
bool effect_particles_same(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack);
bool standalone_effect_target(const std::vector<std::string>& roots, const std::string& filename,
                              std::string* target, std::string* who);
std::string plain_effect_refusal(const std::string& why);

// ---- cosmetic_effect_check.cpp ----
// One effect file judged (check_effect's verdict, without slots and overrides): import_effect's check.
void judge_effect_dat(const std::string& filename, const std::vector<uint8_t>& bytes, EffectVerdict* out);

// ---- cosmetic_catalog.cpp ----
bool costume_online_with_open_disc(const AssetRecord& asset,
                                          const std::vector<uint8_t>& candidate, std::string* detail);

// ---- cosmetic_live.cpp ----
std::string climber_partner(const std::string& target);

// ---- cosmetic_stage_skins.cpp ----
std::string stage_target_locked(const std::string& stage_file);
bool make_standalone_stage(const fs::path& source, const std::string& member,
                           std::vector<uint8_t> bytes, VaultResourcePlan* resource,
                           std::string* error, const std::string& target_override = {});

// ---- cosmetic_looks.cpp ----
std::vector<uint8_t> look_fighter_tables(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack);
bool look_file_safe(const std::string& path, const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack,
                           std::string* why);

}  // namespace host::cosmetics
