// Fighters installed beside the game, for a disc that has no m-ex table of its own (roster.h).

#include "roster.h"
#include "dat_check.h"
#include "host.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>
#include <tuple>

#include "nlohmann/json.hpp"

// The select screen pictures are PNG files: stb_image's reader, kept private to this file. It refuses
// a picture wider or taller than 4096 pixels.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace host::roster {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::vector<Fighter> g_fighters;
uint32_t g_generation = 0;
std::map<std::tuple<size_t, int, int>, Image> g_images;

// fighter.json "functions" name of each m-ex ftFunction table, in table order (m-ex mexLib
// MexFighterFunctions.ToMxDt; tools/mexft/mxdt.py has the same list).
const char* const kFunctions[46] = {
  "onLoad", "onRespawn", "onDestroy", "moveLogicPointer", "specialN", "specialNAir", "specialS", "specialSAir",
  "specialHi", "specialHiAir", "specialLw", "specialLwAir", "onAbsorb", "onItemPickup", "onMakeItemInvisible",
  "onMakeItemVisible", "onItemDrop", "onItemCatch", "onUnknownItemRelated", "onApplyHeadItem", "onRemoveHeadItem",
  "eyeTextureDamaged", "eyeTextureNormal", "onFrame", "onActionStateChange", "resetAttribute", "onModelRender",
  "onShadowRender", "onUnknownMultijump", "onActionStateChangeWhileEyeTextureIsChanged1", "onTwoEntryTable1",
  "enterFloat", "enterDoubleJump", "enterTether", "onLanding", "onSmashF", "onSmashHi", "onSmashLw", "onExtRstAnim",
  "onIndexExtRstAnim", "demoMoveLogicPointer", "onIntroL", "onIntroR", "onAppeal", "onCatch", "getSwordTrail",
};

// fighter.json "boneDefinitions.lookup": the parts of a skeleton table, in the table's order (HSDLib
// SBM_BoneLookupTable).
const char* const kParts[0x36] = {
  "top", "trans", "xRot", "yRot", "hip", "waist", "lLegA", "lLeg", "lKnee", "lFootA", "lFoot", "rLegA", "rLeg", "rKnee",
  "rFootA", "rFoot", "waistB", "bust", "lClavicle", "lShoulderA", "lShoulder", "lArm", "lHand", "lIndex1", "lIndex2",
  "lMiddle1", "lMiddle2", "lRing1", "lRing2", "lPinky1", "lPinky2", "lHave", "lThumb1", "lThumb2", "neckN", "headN",
  "rClavicle", "rShoulderA", "rShoulder", "rArm", "rHand", "rIndex1", "rIndex2", "rMiddle1", "rMiddle2", "rRing1",
  "rRing2", "rPinky1", "rPinky2", "rHave", "rThumb1", "rThumb2", "throw", "extra",
};

// The files a disc carries that a fighter may bring.
bool disc_file(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return ext == "dat" || ext == "usd" || ext == "ssm" || ext == "hps" || ext == "thp" || ext == "mth";
}

// What a record may hold: the joints of a skeleton the game keeps (MAX_FT_PARTS, ft/ftparts.h), the
// costumes taken from a fighter.json, and the entries of a fighter's action table (a bound well past
// retail's largest, 479; tables_problem holds each fighter to its own file).
constexpr uint32_t kMaxJoints = 140, kMaxCostumes = 32, kMaxActions = 2048;
// The buffer the game copies one animation into (fighter_x59C_alloc_data, ft/fighter.c).
constexpr uint32_t kAnimationBuffer = 0x8000;

// A file name the game can open: lbFileGetFullName (lb/lbfile.c) builds the disc name in 32 bytes. A
// name with an extension is kept whole, 31 characters at most; one without (or ending in ".") gets
// ".usd" or ".dat" after its stem, 27 characters at most.
constexpr size_t kMaxGameFileName = 31, kMaxGameFileStem = 27;
bool game_file_name(const std::string& name) {
  const size_t dot = name.find('.');
  if (dot != std::string::npos && dot + 1 < name.size()) return name.size() <= kMaxGameFileName;
  return (dot == std::string::npos ? name.size() : dot) <= kMaxGameFileStem;
}

std::string text(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Why the game's loader would refuse a .dat or .usd file, empty when it would not (or the file is
// of another kind). The loader relocates an archive in place by the offsets the file gives, so one
// that points outside itself must never reach it: each such file is a whole archive, the fighter's
// animation file (fighter.json "animFile") the archives of its animations (host/dat_check.h). Kept
// by path, size and time: the folder is read again at every mode change.
std::string archive_problem(const fs::path& path, const std::string& name, const std::string& anim_file) {
  const std::string low = lower(name);
  if (low.size() < 4 || (low.compare(low.size() - 4, 4, ".dat") != 0 && low.compare(low.size() - 4, 4, ".usd") != 0))
    return {};
  struct Known { uint64_t size; long long time; bool animations; std::string problem; };
  static std::map<std::string, Known> known;
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  const long long time = ec ? 0 : (long long)fs::last_write_time(path, ec).time_since_epoch().count();
  if (ec) return "it cannot be read";
  const bool animations = low == lower(anim_file);
  const auto it = known.find(path.u8string());
  if (it != known.end() && it->second.size == size && it->second.time == time && it->second.animations == animations)
    return it->second.problem;
  std::string problem;
  if (size > 256ull * 1024 * 1024) {
    problem = "it is too large";
  } else {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((size_t)size);
    if (!in || !in.read((char*)bytes.data(), (std::streamsize)bytes.size())) problem = "it cannot be read";
    else problem = animations ? host::dat::animation_file_error(bytes.data(), bytes.size())
                              : host::dat::structure_error(bytes.data(), bytes.size(), host::dat::Rules::Loader);
  }
  known[path.u8string()] = {size, time, animations, problem};
  return problem;
}

// The first disc file of a fighter's folder the game's loader would refuse, said as a reason; empty
// when there is none.
std::string folder_problem(const fs::path& dir, const std::string& anim_file) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if (!it->is_regular_file(ec) || !disc_file(name) || !game_file_name(name)) continue;
    const std::string problem = archive_problem(it->path(), name, anim_file);
    if (!problem.empty()) return name + " is not an archive the game can load (" + problem + ")";
  }
  return {};
}

uint32_t number(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end()) return 0;
  if (it->is_boolean()) return it->get<bool>() ? 1u : 0u;
  if (it->is_number_unsigned()) return (uint32_t)it->get<uint64_t>();
  if (it->is_number_integer()) return (uint32_t)it->get<int64_t>();
  return 0;
}

// The tables the game reads through a fighter's record by count or index must lie in its fighter
// file (fighter.json "fighterDataPath", the ftData of "fighterDataSymbol"): the animCount entries of
// its action table (0x18 bytes each, ftData +0xC), each animation they name inside the animation
// file and no larger than the game's animation buffer, and the costume rows (+0x8: the visibility
// rows of 16 bytes and the texture list of 4) each costume's visibilityIndex names. The game follows
// every word of such a row (row 0's in place of a null one), so each is null or one of the file's
// relocated pointers. Empty when they do. The files have passed folder_problem.
std::string tables_problem(const fs::path& dir, const json& j) {
  const json none = json::object();
  const json& files = j.count("files") && j["files"].is_object() ? j["files"] : none;
  std::error_code ec;
  const fs::path data_path = dir / fs::u8path(text(files, "fighterDataPath"));
  const uint64_t size = fs::file_size(data_path, ec);
  if (ec || size < 0x20 || size > 256ull * 1024 * 1024) return "its fighter file cannot be read";
  std::vector<uint8_t> b((size_t)size);
  {
    std::ifstream in(data_path, std::ios::binary);
    if (!in || !in.read((char*)b.data(), (std::streamsize)b.size())) return "its fighter file cannot be read";
  }
  using host::dat::be32;
  const uint64_t data_size = be32(b.data() + 4), relocs = be32(b.data() + 8), publics = be32(b.data() + 12),
                 externs = be32(b.data() + 16);
  const uint64_t pub = 0x20 + data_size + relocs * 4, strings = pub + (publics + externs) * 8;
  if (strings > size) return "its fighter file is not an archive";
  // Offset `at` of the data block holding `n` bytes, and the word there.
  const auto inside = [&](uint64_t at, uint64_t n) { return at + n <= data_size; };
  const auto word = [&](uint64_t at) { return (uint64_t)be32(b.data() + 0x20 + at); };
  std::vector<uint32_t> relocated((size_t)relocs);
  for (uint64_t i = 0; i < relocs; ++i) relocated[(size_t)i] = be32(b.data() + 0x20 + data_size + i * 4);
  std::sort(relocated.begin(), relocated.end());
  // Whether the word at `at` is one of the file's relocated pointers (or null).
  const auto pointer = [&](uint64_t at) { return std::binary_search(relocated.begin(), relocated.end(), (uint32_t)at); };
  const auto pointer_or_null = [&](uint64_t at) { return word(at) == 0 || pointer(at); };
  const std::string symbol = text(files, "fighterDataSymbol");
  uint64_t root = UINT64_MAX;
  for (uint64_t i = 0; i < publics && root == UINT64_MAX; ++i) {
    const uint64_t name = strings + be32(b.data() + pub + i * 8 + 4);
    if (name + symbol.size() < size && std::memcmp(b.data() + name, symbol.c_str(), symbol.size() + 1) == 0)
      root = be32(b.data() + pub + i * 8);
  }
  if (root == UINT64_MAX || !inside(root, 0x10)) return "its fighter file has no " + symbol;
  if (!pointer(root + 0x8) || !pointer(root + 0xC)) return "its fighter file's " + symbol + " does not point to its tables";
  const uint64_t actions = number(files, "animCount"), table = word(root + 0xC);
  if (!inside(table, actions * 0x18))
    return "its fighter file's action table holds fewer than its animCount (" + std::to_string(actions) + ") entries";
  const uint64_t anim_size = fs::file_size(dir / fs::u8path(text(files, "animFile")), ec);
  if (ec) return "its animFile cannot be read";
  for (uint64_t i = 0; i < actions; ++i) {
    const uint64_t at = word(table + i * 0x18 + 4), length = word(table + i * 0x18 + 8);
    if (length != 0 && (length > kAnimationBuffer || at + length > anim_size))
      return "action " + std::to_string(i) + " names an animation " +
             (length > kAnimationBuffer ? "larger than the game's animation buffer" : "outside its animFile");
  }
  const uint64_t parts = word(root + 0x8);
  if (!inside(parts, 0x10)) return "its fighter file's costume tables are outside it";
  if (!pointer_or_null(parts + 0x4) || !pointer_or_null(parts + 0xC)) return "its fighter file's costume tables are not pointers of it";
  const uint64_t rows = word(parts + 0x4), textures = word(parts + 0xC);
  // Whether row `row` of the tables is in the file and holds nothing but pointers and nulls.
  const auto row_ok = [&](uint64_t row) {
    if ((rows != 0 && !inside(rows, (row + 1) * 16)) || (textures != 0 && !inside(textures, (row + 1) * 4))) return false;
    for (uint64_t k = 0; rows != 0 && k < 4; ++k)
      if (!pointer_or_null(rows + row * 16 + k * 4)) return false;
    return textures == 0 || pointer_or_null(textures + row * 4);
  };
  if (!row_ok(0)) return "its fighter file's first costume row is not one the game can use";
  const json& costumes = j.count("costumes") && j["costumes"].is_array() ? j["costumes"] : none;
  for (size_t c = 0; costumes.is_array() && c < costumes.size() && c < kMaxCostumes; ++c) {
    const json& file = costumes[c].is_object() && costumes[c].count("file") && costumes[c]["file"].is_object()
        ? costumes[c]["file"] : none;
    const uint64_t row = number(file, "visibilityIndex");
    if (!row_ok(row))
      return "costume " + std::to_string(c) + "'s visibilityIndex (" + std::to_string(row) +
             ") is not a row of its fighter file's costume tables";
  }
  return {};
}

struct Writer {
  std::vector<uint8_t> out;
  bool ok = true;
  void u8(uint32_t v) { out.push_back((uint8_t)v); }
  void u16(uint32_t v) { u8(v >> 8); u8(v); }
  void u32(uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
  // A file or symbol name: plain ASCII, at most 255 bytes.
  void str(const std::string& s) {
    if (s.size() > 255 || std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c >= 127; })) {
      ok = false;
      u8(0);
      return;
    }
    u8((uint32_t)s.size());
    out.insert(out.end(), s.begin(), s.end());
  }
};

// The record of one fighter.json, in the order sourceport/game/shim/mu_mex_roster.c reads it.
bool build_record(const json& j, Fighter& fighter, std::string& why) {
  const json none = json::object();
  const json& files = j.count("files") && j["files"].is_object() ? j["files"] : none;
  const json& functions = j.count("functions") && j["functions"].is_object() ? j["functions"] : none;
  Writer w;
  fighter.name = text(j, "name");
  if (fighter.name.empty() || text(files, "fighterDataPath").empty()) { why = "it names no fighter file"; return false; }
  // The whole name is shown and written into files a line at a time (the record keeps 60 characters).
  if (std::any_of(fighter.name.begin(), fighter.name.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
    why = "its name holds a line break or another control character";
    return false;
  }
  // The files the game opens by these names: one longer than it holds would never be found.
  for (const char* key : {"fighterDataPath", "animFile", "rstAnimFile", "effectFile", "kirbyCapFileName", "kirbyEffectFile"}) {
    if (!game_file_name(text(files, key))) {
      why = std::string("its ") + key + " is longer than a game file name (31 characters, 27 without an extension)";
      return false;
    }
  }
  w.u8(1);
  w.str(fighter.name.substr(0, 60));
  w.str(text(files, "fighterDataPath"));
  w.str(text(files, "fighterDataSymbol"));
  w.str(text(files, "animFile"));
  w.str(text(files, "rstAnimFile"));
  w.str(text(files, "effectFile"));
  w.str(text(files, "effectSymbol"));
  w.str(text(files, "kirbyCapFileName"));
  w.str(text(files, "kirbyCapSymbol"));
  w.str(text(files, "kirbyEffectFile"));
  w.str(text(files, "kirbyEffectSymbol"));
  w.str(text(files, "demoResult"));
  w.str(text(files, "demoIntro"));
  w.str(text(files, "demoEnding"));
  w.str(text(files, "demoWait"));
  // The game reads this many entries of the fighter file's action table (tables_problem checks
  // they are in it); retail fighters have 295 to 479.
  const uint32_t actions = number(files, "animCount");
  if (actions == 0 || actions > kMaxActions) {
    why = "its animCount (" + std::to_string(actions) + ") is not 1 to " + std::to_string(kMaxActions);
    return false;
  }
  w.u32(actions);
  w.u8(number(j, "seriesID"));
  w.u8(number(j, "canWallJump"));
  w.u8(std::min<uint32_t>(number(j, "soundBank"), 255));
  w.u32(number(j, "announcerCall"));
  w.u32(number(j, "victoryTheme"));
  {
    float scale = 1.0f;
    const auto it = j.find("resultScreenScale");
    if (it != j.end() && it->is_number()) scale = it->get<float>();
    uint32_t bits;
    std::memcpy(&bits, &scale, 4);
    w.u32(bits);
  }
  const json empty = json::array();
  const json& costumes = j.count("costumes") && j["costumes"].is_array() ? j["costumes"] : empty;
  const size_t costume_count = std::min<size_t>(costumes.size(), kMaxCostumes);
  // The team colors are costumes of this fighter: the game dresses a team member in that costume.
  for (const char* key : {"redCostumeIndex", "blueCostumeIndex", "greenCostumeIndex"}) {
    const uint32_t costume = number(j, key);
    if (costume_count > 0 && costume >= costume_count) {
      why = std::string("its ") + key + " (" + std::to_string(costume) + ") is not one of its " +
            std::to_string(costume_count) + " costumes";
      return false;
    }
    w.u8(costume);
  }
  w.u8((uint32_t)costume_count);
  for (size_t c = 0; c < costume_count; ++c) {
    const json& file = costumes[c].is_object() && costumes[c].count("file") && costumes[c]["file"].is_object()
        ? costumes[c]["file"] : none;
    if (!game_file_name(text(file, "fileName"))) {
      why = "costume " + std::to_string(c) + "'s fileName is longer than a game file name (31 characters, 27 without an extension)";
      return false;
    }
    // A row of the fighter file's costume tables (tables_problem checks the row is in the file).
    const uint32_t row = number(file, "visibilityIndex");
    if (row > 255) {
      why = "costume " + std::to_string(c) + "'s visibilityIndex (" + std::to_string(row) + ") is past 255";
      return false;
    }
    w.str(text(file, "fileName"));
    w.str(text(file, "jointSymbol"));
    w.str(text(file, "materialSymbol"));
    w.u8(row);
  }
  // The articles the fighter's code makes (its blaster shot, its thrown sword): how many. On an
  // m-ex disc each is an item kind MexTool numbered; the game numbers them itself here, so the
  // numbers a fighter.json may carry are not sent.
  {
    const size_t items = j.count("items") && j["items"].is_array() ? std::min<size_t>(j["items"].size(), 64) : 0;
    w.u8((uint32_t)items);
    for (size_t i = 0; i < items; ++i) w.u16(0);
  }
  for (const char* name : kFunctions) w.u32(number(functions, name));
  w.u32(number(functions, "onTwoEntryTable2"));
  // The skeleton (PlCo.dat on an m-ex disc): the joint count, the joint of each part, the extras.
  {
    const json& bones = j.count("boneDefinitions") && j["boneDefinitions"].is_object() ? j["boneDefinitions"] : none;
    const json& lookup = bones.count("lookup") && bones["lookup"].is_object() ? bones["lookup"] : none;
    // The game indexes the fighter's joints by these: each part a joint below the count, or 255
    // (the fighter has no such part).
    const uint32_t joints = number(lookup, "boneCount");
    if (joints > kMaxJoints) {
      why = "its skeleton has " + std::to_string(joints) + " joints, more than the game's " + std::to_string(kMaxJoints);
      return false;
    }
    w.u16(joints);
    for (const char* part : kParts) {
      const uint32_t joint = lookup.count(part) ? number(lookup, part) : 255;
      if (joint != 255 && joint >= joints) {
        why = std::string("its skeleton puts part ") + part + " at joint " + std::to_string(joint) + " of " +
              std::to_string(joints);
        return false;
      }
      w.u8(joint);
    }
    const json& ext = bones.count("ext") && bones["ext"].is_array() ? bones["ext"] : empty;
    const size_t ext_count = std::min<size_t>(ext.size(), 64);
    w.u8((uint32_t)ext_count);
    for (size_t i = 0; i < ext_count; ++i)
      for (const char* key : {"x00", "x01", "x02", "x03"}) w.u8(ext[i].is_object() ? number(ext[i], key) : 0);
  }
  if (!w.ok) { why = "a file or symbol name in it is not plain text"; return false; }
  if (w.out.size() > 4000) { why = "it is too large for one record"; return false; }
  fighter.record = std::move(w.out);
  return true;
}

uint32_t fnv1a(uint32_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// The pictures a fighter folder may hold, with their size and time: a changed picture is a changed set.
uint32_t picture_hash(uint32_t h, const fs::path& dir) {
  std::error_code ec;
  std::vector<std::string> lines;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if (!it->is_regular_file(ec) || name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0) continue;
    const auto time = it->last_write_time(ec).time_since_epoch().count();
    lines.push_back(name + ":" + std::to_string(it->file_size(ec)) + ":" + std::to_string((long long)time));
  }
  std::sort(lines.begin(), lines.end());
  for (const auto& line : lines) h = fnv1a(h, (const uint8_t*)line.data(), line.size());
  return h;
}

// width x height RGBA pixels of a PNG file, resampled to that size; empty when it cannot be read.
std::vector<uint8_t> read_png(const fs::path& path, int width, int height) {
  std::vector<uint8_t> out;
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (file.empty() || file.size() > 16u * 1024 * 1024) return out;
  int w = 0, h = 0, n = 0;
  // The size comes from the header first, so a picture too large is refused before any pixels are
  // decoded.
  if (!stbi_info_from_memory(file.data(), (int)file.size(), &w, &h, &n)) return out;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return out;
  stbi_uc* px = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
  if (!px) return out;
  if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
    out.resize((size_t)width * height * 4);
    if (w == width && h == height) {
      std::memcpy(out.data(), px, out.size());
    } else {
      // Another size: each pixel is the average of the source pixels it covers (or the nearest one).
      for (int y = 0; y < height; ++y) {
        const int y0 = y * h / height, y1 = std::max(y0 + 1, (y + 1) * h / height);
        for (int x = 0; x < width; ++x) {
          const int x0 = x * w / width, x1 = std::max(x0 + 1, (x + 1) * w / width);
          uint32_t sum[4] = {0, 0, 0, 0}, count = 0;
          for (int sy = y0; sy < y1 && sy < h; ++sy)
            for (int sx = x0; sx < x1 && sx < w; ++sx, ++count)
              for (int c = 0; c < 4; ++c) sum[c] += px[((size_t)sy * w + sx) * 4 + c];
          for (int c = 0; c < 4; ++c) out[((size_t)y * width + x) * 4 + c] = (uint8_t)(count ? sum[c] / count : 0);
        }
      }
    }
  }
  stbi_image_free(px);
  return out;
}

// A plain tile for a fighter that brings no picture: dark, with a lighter edge.
std::vector<uint8_t> placeholder(int width, int height) {
  std::vector<uint8_t> out((size_t)width * height * 4);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const bool edge = x < 2 || y < 2 || x >= width - 2 || y >= height - 2;
      uint8_t* p = &out[((size_t)y * width + x) * 4];
      p[0] = edge ? 150 : 52; p[1] = edge ? 150 : 56; p[2] = edge ? 160 : 68; p[3] = 255;
    }
  return out;
}

// RGBA pixels as the console's RGBA8 texture: 4x4 blocks, each its 16 alpha and red bytes, then its
// 16 green and blue bytes. The width and height are multiples of 4.
std::vector<uint8_t> gx_rgba8(const std::vector<uint8_t>& px, int width, int height) {
  std::vector<uint8_t> out;
  out.reserve(px.size());
  for (int by = 0; by < height; by += 4)
    for (int bx = 0; bx < width; bx += 4) {
      for (int i = 0; i < 16; ++i) {
        const uint8_t* p = &px[((size_t)(by + i / 4) * width + bx + i % 4) * 4];
        out.push_back(p[3]); out.push_back(p[0]);
      }
      for (int i = 0; i < 16; ++i) {
        const uint8_t* p = &px[((size_t)(by + i / 4) * width + bx + i % 4) * 4];
        out.push_back(p[1]); out.push_back(p[2]);
      }
    }
  return out;
}

}   // namespace

bool scan(const fs::path& folder) {
  std::vector<Fighter> found;
  std::error_code ec;
  std::vector<fs::path> folders;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) && fs::is_regular_file(it->path() / "fighter.json", ec)) folders.push_back(it->path());
  }
  std::sort(folders.begin(), folders.end());   // the same order, and so the same ids, on every computer
  for (const auto& dir : folders) {
    Fighter fighter;
    fighter.folder = dir.filename().u8string();
    fighter.path = dir;
    std::string why;
    try {
      std::ifstream in(dir / "fighter.json", std::ios::binary);
      const json j = json::parse(in);
      if (!j.is_object()) why = "it is not a fighter";
      else if (build_record(j, fighter, why)) {
        // A folder copied by hand gets the check an installed package had.
        const json none = json::object();
        why = folder_problem(dir, text(j.count("files") && j["files"].is_object() ? j["files"] : none, "animFile"));
        if (why.empty()) why = tables_problem(dir, j);
        if (!why.empty()) fighter.record.clear();
      }
    } catch (const std::exception& e) {
      why = e.what();
      fighter.record.clear();
    }
    if (fighter.record.empty()) {
      static std::vector<std::string> said;
      if (std::find(said.begin(), said.end(), fighter.folder) == said.end()) {
        said.push_back(fighter.folder);
        host::log("roster: fighters\\%s\\fighter.json is not used (%s)", fighter.folder.c_str(), why.c_str());
      }
      continue;
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = it->path().filename().u8string();
      // A file whose own name is longer than the game opens is never asked for: not served.
      if (!it->is_regular_file(ec) || !disc_file(name) || !game_file_name(name)) continue;
      const uint64_t size = it->file_size(ec);
      if (ec || size == 0 || size > 256ull * 1024 * 1024) continue;
      fighter.files.push_back({name, it->path(), size});
    }
    std::sort(fighter.files.begin(), fighter.files.end(), [](const File& a, const File& b) { return a.name < b.name; });
    found.push_back(std::move(fighter));
  }
  uint32_t hash = 2166136261u;
  for (const auto& fighter : found) {
    hash = fnv1a(hash, fighter.record.data(), fighter.record.size());
    for (const auto& file : fighter.files) {
      hash = fnv1a(hash, (const uint8_t*)file.name.data(), file.name.size());
      hash = fnv1a(hash, (const uint8_t*)&file.size, sizeof file.size);
    }
    hash = picture_hash(hash, fighter.path);
    // Its own sounds (sound\, host/fighter_sound.h): a changed bank or call is a changed set.
    std::vector<std::string> lines;
    for (fs::directory_iterator it(fighter.path / "sound", ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const auto time = it->last_write_time(ec).time_since_epoch().count();
      lines.push_back(it->path().filename().u8string() + ":" + std::to_string(it->file_size(ec)) + ":" +
                      std::to_string((long long)time));
    }
    ec.clear();
    std::sort(lines.begin(), lines.end());
    for (const auto& line : lines) hash = fnv1a(hash, (const uint8_t*)line.data(), line.size());
  }
  if (found.empty()) hash = 0;
  else if (hash == 0) hash = 1;
  const bool changed = hash != g_generation;
  if (changed) g_images.clear();
  g_fighters = std::move(found);
  g_generation = hash;
  return changed;
}

const std::vector<Fighter>& fighters() { return g_fighters; }
uint32_t generation() { return g_generation; }

namespace {

// One file of a package into the folder being made, under its own plain name.
bool take(const fs::path& from, const fs::path& stage, const std::string& name, uint64_t* total) {
  std::error_code ec;
  const uint64_t size = fs::file_size(from, ec);
  if (ec || size == 0 || size > 256ull * 1024 * 1024 || *total + size > 1024ull * 1024 * 1024) return false;
  if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name[0] == '.') return false;
  fs::copy_file(from, stage / fs::u8path(name), fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  *total += size;
  return true;
}

}   // namespace

bool install_package(const fs::path& unpacked, const fs::path& fighters_folder,
                     const std::function<bool(const fs::path&, const fs::path&)>& unzip, Installed* out,
                     std::string* problem) {
  std::error_code ec;
  // fighter.json: at the top of the package, or in the one folder a zip of a folder has.
  fs::path base = unpacked;
  if (!fs::is_regular_file(base / "fighter.json", ec)) {
    base.clear();
    for (fs::directory_iterator it(unpacked, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec) && fs::is_regular_file(it->path() / "fighter.json", ec)) { base = it->path(); break; }
    }
  }
  if (base.empty()) { *problem = "This is not a fighter package (no fighter.json)"; return false; }
  json j;
  Fighter checked;
  std::string why;
  try {
    std::ifstream in(base / "fighter.json", std::ios::binary);
    j = json::parse(in);
  } catch (const std::exception&) {
    *problem = "The package's fighter.json cannot be read";
    return false;
  }
  if (!j.is_object() || !build_record(j, checked, why)) {
    *problem = "The package's fighter.json is not usable: " + (why.empty() ? std::string("it is not a fighter") : why);
    return false;
  }
  std::string slug;
  for (const char c : checked.name) {
    if (std::isalnum((unsigned char)c)) slug += (char)std::tolower((unsigned char)c);
    else if (!slug.empty() && slug.back() != '_') slug += '_';
  }
  while (!slug.empty() && slug.back() == '_') slug.pop_back();
  if (slug.size() > 40) slug.resize(40);
  if (slug.empty()) slug = "fighter";
  const fs::path target = fighters_folder / fs::u8path(slug);
  const fs::path stage = fighters_folder / fs::u8path(slug + ".part");
  fs::remove_all(stage, ec);
  fs::create_directories(stage, ec);
  if (ec) { *problem = "The fighters folder cannot be written"; return false; }

  // The costume a file name is, by fighter.json's list.
  std::vector<std::string> costume_files;
  if (j.count("costumes") && j["costumes"].is_array()) {
    for (const auto& c : j["costumes"]) {
      const json none = json::object();
      const json& file = c.is_object() && c.count("file") && c["file"].is_object() ? c["file"] : none;
      costume_files.push_back(lower(text(file, "fileName")));
    }
  }
  uint64_t total = 0;
  // A disc file whose own name is longer than the game opens could never be loaded: left out.
  int files = 0, portraits = 0, long_names = 0;
  bool ok = take(base / "fighter.json", stage, "fighter.json", &total);
  std::vector<fs::path> zips;
  for (fs::directory_iterator it(base, ec), end; ok && !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().u8string(), low = lower(name);
    if (disc_file(name) && !game_file_name(name)) ++long_names;
    else if (disc_file(name)) { ok = take(it->path(), stage, name, &total); ++files; }
    else if (low == "icon.png") ok = take(it->path(), stage, "icon.png", &total);
    else if ((low.rfind("csp_", 0) == 0 || low.rfind("stc_", 0) == 0) && low.size() > 8 &&
             low.compare(low.size() - 4, 4, ".png") == 0) {
      ok = take(it->path(), stage, low, &total);
      if (low[0] == 'c') ++portraits;
    }
    else if (low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0 && low != "sound.zip" && zips.size() < 64)
      zips.push_back(it->path());
  }
  // A costume's zip: its file, and the pictures that go with it.
  std::sort(zips.begin(), zips.end());
  for (size_t z = 0; ok && z < zips.size(); ++z) {
    const fs::path dest = unpacked / fs::u8path("costume_" + std::to_string(z));
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (!unzip(zips[z], dest)) continue;   // a zip that is not a costume's is left out
    int costume = -1;
    fs::path csp, stc;
    for (fs::directory_iterator it(dest, ec), end; ok && !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string name = it->path().filename().u8string(), low = lower(name);
      if (disc_file(name) && !game_file_name(name)) {
        ++long_names;
      } else if (disc_file(name)) {
        ok = take(it->path(), stage, name, &total);
        ++files;
        for (size_t c = 0; c < costume_files.size(); ++c)
          if (costume_files[c] == low) costume = (int)c;
      } else if (low == "csp.png") csp = it->path();
      else if (low == "stc.png") stc = it->path();
    }
    if (ok && costume >= 0) {
      if (!csp.empty() && take(csp, stage, "csp_" + std::to_string(costume) + ".png", &total)) ++portraits;
      if (!stc.empty()) take(stc, stage, "stc_" + std::to_string(costume) + ".png", &total);
    }
    fs::remove_all(dest, ec);   // copied: one costume's files on the disk at a time
  }
  std::string archive_trouble;
  if (ok && !fs::is_regular_file(stage / fs::u8path(text(j["files"], "fighterDataPath")), ec)) {
    *problem = "The package does not hold the fighter file its fighter.json names";
    ok = false;
  } else if (!ok) {
    *problem = "A file of the package could not be copied (too large, or the disk is full)";
  } else if (!(archive_trouble = folder_problem(stage, text(j["files"], "animFile"))).empty()) {
    *problem = "The package's " + archive_trouble;
    ok = false;
  } else if (!(archive_trouble = tables_problem(stage, j)).empty()) {
    *problem = "The package's fighter.json does not match its files: " + archive_trouble;
    ok = false;
  }
  if (!ok) { fs::remove_all(stage, ec); return false; }
  fs::remove_all(target, ec);
  fs::rename(stage, target, ec);
  if (ec) { *problem = "The fighter's folder could not be replaced"; fs::remove_all(stage, ec); return false; }
  out->name = checked.name;
  out->folder = slug;
  out->files = files;
  out->portraits = portraits;
  out->base = base;
  out->has_code = fs::is_directory(base / "code", ec);
  const std::string left_out =
      long_names ? ", " + std::to_string(long_names) + " files left out (names longer than the game opens)" : std::string();
  host::log("roster: installed %s as fighters\\%s: %d disc files, %d portraits%s%s", checked.name.c_str(), slug.c_str(), files,
            portraits, left_out.c_str(), out->has_code ? ", with its C source" : "");
  return true;
}

const Image* image(size_t fighter, int what, int index) {
  if (fighter >= g_fighters.size() || what < 0 || what > 2 || index < 0 || index > 255) return nullptr;
  const auto key = std::make_tuple(fighter, what, what == 0 ? 0 : index);
  const auto it = g_images.find(key);
  if (it != g_images.end()) return &it->second;
  const int width = what == 0 ? 64 : what == 1 ? 136 : 24, height = what == 0 ? 56 : what == 1 ? 188 : 24;
  const Fighter& f = g_fighters[fighter];
  const std::string name = what == 0 ? "icon.png" : (what == 1 ? "csp_" : "stc_") + std::to_string(index) + ".png";
  std::vector<uint8_t> px = read_png(f.path / name, width, height);
  // A costume without a picture of its own shows the first costume's.
  if (px.empty() && what != 0 && index != 0) px = read_png(f.path / (what == 1 ? "csp_0.png" : "stc_0.png"), width, height);
  // No icon (a classic MexTool package has none): the top of the first portrait, the icon's shape.
  if (px.empty() && what == 0) {
    const std::vector<uint8_t> csp = read_png(f.path / "csp_0.png", 136, 188);
    if (!csp.empty()) {
      constexpr int kTop = 136 * 56 / 64;   // 119 rows of the portrait: the icon's 64:56
      px.resize((size_t)width * height * 4);
      for (int y = 0; y < height; ++y) {
        const int y0 = y * kTop / height, y1 = std::max(y0 + 1, (y + 1) * kTop / height);
        for (int x = 0; x < width; ++x) {
          const int x0 = x * 136 / width, x1 = std::max(x0 + 1, (x + 1) * 136 / width);
          uint32_t sum[4] = {0, 0, 0, 0}, count = 0;
          for (int sy = y0; sy < y1; ++sy)
            for (int sx = x0; sx < x1; ++sx, ++count)
              for (int c = 0; c < 4; ++c) sum[c] += csp[((size_t)sy * 136 + sx) * 4 + c];
          for (int c = 0; c < 4; ++c) px[((size_t)y * width + x) * 4 + c] = (uint8_t)(sum[c] / count);
        }
      }
      host::log("roster: fighters\\%s has no icon.png: the top of its first portrait is its icon", f.folder.c_str());
    }
  }
  // No stock icon at all: none is handed over, and the game keeps a retail one.
  if (px.empty() && what == 2) { g_images[key]; return &g_images[key]; }
  if (px.empty()) {
    host::log("roster: fighters\\%s has no %s: a plain picture is shown", f.folder.c_str(), name.c_str());
    px = placeholder(width, height);
  }
  Image& made = g_images[key];
  made.width = (uint16_t)width;
  made.height = (uint16_t)height;
  made.data = gx_rgba8(px, width, height);
  return &made;
}

std::vector<uint8_t> console_picture(const std::string& png_utf8, int width, int height) {
  if (png_utf8.empty() || width <= 0 || height <= 0 || width % 4 || height % 4 || width > 1024 || height > 1024) return {};
  const std::vector<uint8_t> px = read_png(fs::u8path(png_utf8), width, height);
  if (px.empty()) return {};
  return gx_rgba8(px, width, height);
}

}   // namespace host::roster
