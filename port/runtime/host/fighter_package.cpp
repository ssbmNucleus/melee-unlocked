// A custom fighter from SSBM Nucleus, installed for this port (fighter_package.h).

#include "fighter_package.h"

#include "archive_unpack.h"
#include "fighter_sound.h"
#include "host.h"
#include "roster.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <system_error>
#include <tuple>
#include <vector>

#include "nlohmann/json.hpp"

namespace host::fighter_package {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// The retail game's code and data on the console (NTSC 1.02): an address a package gives inside it is
// the game's own function or table, one outside it is code a mod brought (mu_ak_fighters.c).
bool retail_address(uint64_t a) { return a >= 0x80003100u && a < 0x804DEC00u; }
bool address(uint64_t a) { return a >= 0x80000000u && a <= 0xFFFFFFFFu; }

// The fighters a package can be made from, by internal kind: their onLoad and move table as m-ex names
// them (sourceport/game/shim/mu_mex_retail.inc), their sound bank (lbaudio_ax ssm_files), and their move
// table's rows with an FNV-1a hash of them as fighter.yml writes them (tools/fighter_donor_tables.py).
struct Donor {
  const char* name;
  uint32_t on_load, move_table;
  uint8_t bank;
  uint16_t rows;
  uint32_t hash;
};
constexpr Donor kDonors[] = {
  {"Mario", 0x800E0960u, 0x803C7120u, 18, 10, 0x509BC76Du},
  {"Fox", 0x800E57ACu, 0x803C7788u, 11, 35, 0x1DEF3C7Au},
  {"Captain Falcon", 0x800E2AECu, 0x803C72B8u, 6, 23, 0xFAE28DE2u},
  {"Donkey Kong", 0x8010D9ACu, 0x803CB838u, 8, 46, 0x65C93039u},
  {"Kirby", 0x800EE680u, 0x803C8368u, 14, 203, 0xC43B540Cu},
  {"Bowser", 0x80132ABCu, 0x803CEDC0u, 15, 23, 0x314C5691u},
  {"Link", 0x800EAE44u, 0x803C7E18u, 16, 21, 0x5854FBA0u},
  {"Sheik", 0x801100ECu, 0x803CC060u, 27, 24, 0x49A0FCB4u},
  {"Ness", 0x8011480Cu, 0x803CC650u, 21, 36, 0x91645F74u},
  {"Peach", 0x8011B628u, 0x803CCCB8u, 22, 30, 0x1A6B6A79u},
  {"Popo", 0x8011EF3Cu, 0x803CD2D0u, 13, 26, 0xF38B19C5u},
  {"Nana", 0x80122EDCu, 0x803CD838u, 13, 26, 0xF38B19C5u},
  {"Pikachu", 0x801243E4u, 0x803CDD78u, 24, 26, 0xD52589D6u},
  {"Samus", 0x8012837Cu, 0x803CE2D0u, 26, 18, 0x5DD0BC2Du},
  {"Yoshi", 0x8012B99Cu, 0x803CE6D0u, 28, 28, 0x3989ED6Du},
  {"Jigglypuff", 0x8013C67Cu, 0x803CFEF0u, 25, 32, 0xED5B0B9Cu},
  {"Mewtwo", 0x80144E48u, 0x803D0B00u, 20, 20, 0xB43A3402u},
  {"Luigi", 0x80142324u, 0x803D0628u, 17, 18, 0xDDE2B2BAu},
  {"Marth", 0x801364ACu, 0x803CF420u, 19, 32, 0x36759ECFu},
  {"Zelda", 0x80139334u, 0x803CFA58u, 27, 18, 0x86FE2165u},
  {"Young Link", 0x80148CE4u, 0x803D0FA0u, 7, 21, 0x0141FF69u},
  {"Dr. Mario", 0x801494E4u, 0x803D1498u, 9, 10, 0xE782E5B4u},
  {"Falco", 0x80149CC4u, 0x803D1848u, 10, 35, 0xC16340DAu},
  {"Pichu", 0x80149E34u, 0x803D1EA8u, 23, 26, 0xD52589D6u},
  {"Mr. Game & Watch", 0x8014A37Cu, 0x803D23E8u, 29, 40, 0x750D873Au},
  {"Ganondorf", 0x8014EE1Cu, 0x803D29F8u, 30, 23, 0x539B28D1u},
  {"Roy", 0x8014F124u, 0x803D2E80u, 31, 32, 0x36759ECFu},
};
constexpr int kDonorCount = (int)(sizeof kDonors / sizeof kDonors[0]);
constexpr uint32_t kRetailBanks = 55;   // bank ids below it are retail banks; 55 is none (mu_mex_roster.c)

// What a package may unpack to, with room to spare (mod_browser.cpp's fighter packages): a real one is
// 20 to 60 files and 7 to 21 MB; a costume's zip 3 files and 2 MB.
constexpr host::archive::Limits kPackageLimits{600, 64ull << 20, 512ull << 20, 250};
constexpr host::archive::Limits kCostumeLimits{16, 64ull << 20, 64ull << 20, 250};

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

uint64_t number(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end()) return 0;
  if (it->is_number_unsigned()) return it->get<uint64_t>();
  if (it->is_number_integer()) return (uint64_t)it->get<int64_t>() & 0xFFFFFFFFull;
  if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
  return 0;
}

// ---- fighter.yml (MexTool's YAML: two-space indents, lists of mappings, plain scalars) ----

struct YLine { size_t indent; std::string text; };

json yml_scalar(std::string v) {
  while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
  if (v.empty() || v == "~" || v == "null") return nullptr;
  if (v == "[]") return json::array();
  if (v == "{}") return json::object();
  if (v == "true") return true;
  if (v == "false") return false;
  static const std::regex integer("-?[0-9]{1,19}"), real("-?[0-9]+\\.[0-9]*([eE][-+]?[0-9]+)?");
  if (std::regex_match(v, integer)) {
    try { return (int64_t)std::stoll(v); } catch (...) { return v; }
  }
  if (std::regex_match(v, real)) {
    try { return std::stod(v); } catch (...) { return v; }
  }
  if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) return v.substr(1, v.size() - 2);
  return v;
}

struct YReader {
  std::vector<YLine> lines;
  size_t pos = 0;
  int depth = 0;
  bool bad = false;

  json block(size_t indent) {
    if (++depth > 32) { bad = true; --depth; return nullptr; }
    json out;
    if (pos < lines.size() && lines[pos].text[0] == '-' && lines[pos].indent >= indent) {
      out = json::array();
      const size_t ind = lines[pos].indent;
      while (pos < lines.size() && lines[pos].indent == ind && lines[pos].text[0] == '-' && !bad) {
        std::string body = lines[pos].text.substr(1);
        while (!body.empty() && body.front() == ' ') body.erase(body.begin());
        if (body.find(':') != std::string::npos && body[0] != '"' && body[0] != '\'') {
          lines[pos] = {ind + 2, body};   // "- key: v": a mapping whose first key is on the dash's line
          out.push_back(mapping(ind + 2));
        } else {
          ++pos;
          out.push_back(yml_scalar(body));
        }
      }
    } else {
      out = mapping(indent);
    }
    --depth;
    return out;
  }

  json mapping(size_t indent) {
    json out = json::object();
    if (pos >= lines.size() || lines[pos].indent < indent) return out;
    const size_t ind = lines[pos].indent;
    while (pos < lines.size() && lines[pos].indent == ind && lines[pos].text[0] != '-' && !bad) {
      const std::string& text = lines[pos].text;
      const size_t colon = text.find(':');
      if (colon == std::string::npos) { bad = true; break; }
      std::string key = text.substr(0, colon), value = text.substr(colon + 1);
      while (!key.empty() && key.back() == ' ') key.pop_back();
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      ++pos;
      // An anchor ("&o0", newer MexTool's songs: a mapping named for a later "*o0") and an alias.
      std::string anchor;
      if (!value.empty() && value[0] == '&') {
        const size_t space = value.find(' ');
        anchor = value.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        value = space == std::string::npos ? std::string() : value.substr(space + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      }
      if (!value.empty() && value[0] == '*' && value.find(' ') == std::string::npos) {
        const auto it = anchors.find(value.substr(1));
        out[key] = it != anchors.end() ? it->second : json(nullptr);
      } else if (!value.empty()) out[key] = yml_scalar(value);
      else if (pos < lines.size() && (lines[pos].indent > ind || (lines[pos].indent == ind && lines[pos].text[0] == '-')))
        out[key] = block(lines[pos].indent);
      else out[key] = nullptr;
      if (!anchor.empty() && anchors.size() < 256) anchors[anchor] = out[key];
    }
    return out;
  }

  std::map<std::string, json> anchors;
};

bool parse_yml(const std::string& text, json* out, std::string* why) {
  if (text.size() > (8u << 20)) { *why = "fighter.yml is too large"; return false; }
  YReader r;
  size_t start = 0;
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) start = 3;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    for (auto& c : line) if (c == '\t') c = ' ';
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    if (indent == line.size() || line[indent] == '#') continue;
    if (line.compare(indent, 3, "---") == 0) continue;
    r.lines.push_back({indent, line.substr(indent)});
    if (r.lines.size() > 400000) { *why = "fighter.yml is too long"; return false; }
  }
  if (r.lines.empty()) { *why = "fighter.yml is empty"; return false; }
  *out = r.mapping(0);
  if (r.bad || r.pos != r.lines.size()) {
    *why = "fighter.yml cannot be read past \"" +
           (r.pos < r.lines.size() ? r.lines[r.pos].text.substr(0, 60) : std::string("its end")) + "\"";
    return false;
  }
  return true;
}

std::string str(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) return {};
  if (it->is_string()) return it->get<std::string>();
  if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
  return {};
}

// fighter.yml -> fighter.json, as the SSBM Nucleus desktop app converts one
// (vault/importing/fighter_yml_converter.py): the keys the roster reads, the move table a classic package
// embeds, and its articles.
bool convert(const json& y, json* out, std::string* why) {
  if (!y.is_object()) { *why = "fighter.yml is not a fighter"; return false; }
  json files = json::object();
  for (const char* key : {"fighterDataPath", "fighterDataSymbol", "animFile", "demoFile", "demoWait", "demoResult", "demoIntro",
                          "demoEnding", "rstAnimFile", "effectFile", "effectSymbol", "kirbyCapFileName", "kirbyCapSymbol",
                          "kirbyEffectFile", "kirbyEffectSymbol"})
    files[key] = str(y, key);
  files["animCount"] = number(y, "animCount");
  files["rstAnimCount"] = number(y, "rstAnimCount");
  if (files["fighterDataPath"].get<std::string>().empty() || files["animFile"].get<std::string>().empty()) {
    *why = "fighter.yml names no fighter file";
    return false;
  }
  const json none = json::object();
  const json& yfun = y.contains("functions") && y["functions"].is_object() ? y["functions"] : none;
  json functions = json::object();
  for (const auto& [key, value] : yfun.items()) {
    if (key == "moveLogic" || !(value.is_number_integer() || value.is_number_unsigned())) continue;
    functions[key == "onItemRelease" ? "onItemDrop" : key] = number(yfun, key.c_str());
  }
  json moves = json::array();
  if (yfun.contains("moveLogic") && yfun["moveLogic"].is_array())
    for (const auto& m : yfun["moveLogic"]) {
      if (!m.is_object()) continue;
      json row = json::object();
      for (const char* key : {"animationID", "stateFlags", "attackID", "bitFlags", "animationCallBack", "iASACallBack",
                              "physicsCallback", "collisionCallback", "cameraCallback"})
        row[key] = number(m, key);
      moves.push_back(row);
    }
  json lookup = json::object();
  const json& bones = y.contains("boneTable") && y["boneTable"].is_object() ? y["boneTable"] : none;
  for (const auto& [key, value] : bones.items())
    if (value.is_number_integer()) lookup[key] = value.get<int64_t>();
  json costumes = json::array();
  if (y.contains("costumes") && y["costumes"].is_array())
    for (const auto& c : y["costumes"]) {
      if (!c.is_object() || str(c, "fileName").empty()) continue;
      const std::string file = str(c, "fileName");
      costumes.push_back({{"name", fs::u8path(file).stem().u8string()},
                          {"file", {{"fileName", file}, {"jointSymbol", str(c, "modelSymbol")},
                                    {"materialSymbol", str(c, "materialSymbol")},
                                    {"visibilityIndex", number(c, "visibilityIndex")}}}});
    }
  if (costumes.empty()) { *why = "fighter.yml has no costumes"; return false; }
  std::string name = str(y, "nameText");
  if (name.empty()) name = "Fighter";
  json j = {
    {"name", name},
    {"seriesID", number(y, "insigniaID")},
    {"canWallJump", y.contains("canWallJump") && y["canWallJump"].is_boolean() && y["canWallJump"].get<bool>()},
    {"soundBank", kRetailBanks},
    {"ssmBitfield1", number(y, "sSMBitfield1")},
    {"ssmBitfield2", number(y, "sSMBitfield2")},
    // As the desktop app's install: the victory theme its default, no announcer call (neither is
    // carried by the roster's table: mu_mex_roster.c).
    {"announcerCall", 0},
    {"victoryTheme", 10},
    {"resultScreenScale", y.contains("resultScreenScale") && y["resultScreenScale"].is_number() ? y["resultScreenScale"].get<double>() : 1.0},
    {"redCostumeIndex", number(y, "redCostumeIndex")},
    {"blueCostumeIndex", number(y, "blueCostumeIndex")},
    {"greenCostumeIndex", number(y, "greenCostumeIndex")},
    {"items", y.contains("items") && y["items"].is_array() ? y["items"] : json::array()},
    {"boneDefinitions", {{"lookup", lookup}, {"ext", json::array()}}},
    {"costumes", costumes},
    {"files", files},
    {"functions", functions},
    {"moveLogic", moves},
  };
  *out = std::move(j);
  return true;
}

uint32_t fnv_word(uint32_t h, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) { h ^= (uint8_t)(v >> s); h *= 16777619u; }
  return h;
}

// ---- a package's files ----

bool read_file(const fs::path& path, std::vector<uint8_t>* out, uint64_t limit) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  out->resize((size_t)size);
  return in && (size == 0 || in.read((char*)out->data(), (std::streamsize)size));
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Whether an archive (.dat) exports one of these symbols: 1, 0, or -1 when it is not an archive.
int exports_code(const std::vector<uint8_t>& b) {
  if (b.size() < 0x20) return -1;
  const uint64_t data_size = be32(b.data() + 4), relocs = be32(b.data() + 8), publics = be32(b.data() + 12),
                 externs = be32(b.data() + 16);
  const uint64_t table = 0x20 + data_size + relocs * 4, names = table + (publics + externs) * 8;
  if (publics > 4096 || externs > 4096 || names > b.size()) return -1;
  for (uint64_t i = 0; i < publics; ++i) {
    const uint64_t at = names + be32(b.data() + table + i * 8 + 4);
    if (at >= b.size()) return -1;
    const char* s = (const char*)b.data() + at;
    const size_t room = b.size() - (size_t)at;
    const size_t n = strnlen(s, room);
    const std::string symbol(s, n);
    if (symbol == "ftFunction" || symbol == "itFunction") return 1;
  }
  return 0;
}

// A file of `dir` by its name, whatever its case.
fs::path find_file(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  const std::string want = lower(name);
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && lower(it->path().filename().u8string()) == want) return it->path();
  return {};
}

// Where a fighter sits in a package: a folder of it ("" the top) holding fighter.json or fighter.yml,
// or a zip inside it and the folder in that zip ("Inner.zip|", "Inner.zip|Folder/").
struct Spot {
  std::string zip;        // the inner zip, relative to the package, '/' between folders; "" none
  std::string folder;     // in the package (zip empty) or in the inner zip; "" or ending in '/'
  bool yml = false;
  std::string id() const { return zip.empty() ? folder : zip + "|" + folder; }
};

std::vector<Spot> find_spots(const fs::path& unpacked) {
  std::vector<Spot> spots;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(unpacked, ec), end; !ec && it != end; it.increment(ec)) {
    if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
    if (!it->is_regular_file(ec)) continue;
    const fs::path rel = fs::relative(it->path(), unpacked, ec);
    std::string rel_text = rel.generic_u8string();
    const std::string low = lower(it->path().filename().u8string());
    if (low == "fighter.json" || low == "fighter.yml") {
      Spot s;
      s.folder = rel_text.substr(0, rel_text.size() - low.size());
      s.yml = low == "fighter.yml";
      spots.push_back(s);
    } else if (low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0 && it->file_size(ec) < (256ull << 20)) {
      host::archive::Zip inner;
      std::string error;
      if (!inner.open(it->path(), kPackageLimits, &error)) continue;
      for (const auto& e : inner.entries()) {
        const std::string name = lower(e.name);
        const size_t slash = name.rfind('/');
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        if ((base != "fighter.json" && base != "fighter.yml") || std::count(name.begin(), name.end(), '/') > 1) continue;
        Spot s;
        s.zip = rel_text;
        s.folder = e.name.substr(0, e.name.size() - base.size());
        s.yml = base == "fighter.yml";
        spots.push_back(s);
      }
    }
  }
  return spots;
}

// The spot to install: the site's, else not one marked old, a fighter.json before a fighter.yml, the
// shallowest, then by name.
const Spot* choose(const std::vector<Spot>& spots, const std::string& hint) {
  for (const auto& s : spots)
    if (!hint.empty() && s.id() == hint) return &s;
  if (spots.empty()) return nullptr;
  static const std::regex old_word("(^|[^a-z])old([^a-z]|$)");
  const auto old = [](const Spot& s) { return std::regex_search(lower(s.id()), old_word); };
  const auto depth = [](const Spot& s) {
    const std::string id = s.id();
    return (int)std::count(id.begin(), id.end(), '/') + (s.zip.empty() ? 0 : 1);
  };
  const Spot* best = nullptr;
  for (const auto& s : spots) {
    if (!best) { best = &s; continue; }
    const auto key = [&](const Spot& x) { return std::make_tuple(old(x), x.yml, depth(x), x.id()); };
    if (key(s) < key(*best)) best = &s;
  }
  return best;
}

std::string now_text() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_s(&tm, &t);
  char out[32];
  std::strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return out;
}

}  // namespace

std::string needs_plugin(const Facts& f) {
  if (f.own_code_file) return "its fighter file carries code of its own (ftFunction)";
  if (f.functions_outside > 0) return std::to_string(f.functions_outside) + " of its functions are code of its own";
  if (f.moves_outside > 0) return std::to_string(f.moves_outside) + " of its move callbacks are code of its own";
  if (f.code_folder) return "it brings C source or a plugin of its own";
  if (f.items > 0) return std::to_string(f.items) + " articles of its own";
  if (f.own_move_table) return "its move table is not the one of the fighter it is made from";
  if (f.donor_unknown) return "it is made from no fighter this game has";
  return {};
}

int donor_of(uint32_t on_load) {
  for (int k = 0; k < kDonorCount; ++k)
    if (kDonors[k].on_load == on_load) return k;
  return -1;
}

const char* donor_name(int kind) { return kind >= 0 && kind < kDonorCount ? kDonors[kind].name : ""; }

bool yml_to_json(const std::string& yml, std::string* json_text, std::string* why) {
  json y, j;
  if (!parse_yml(yml, &y, why) || !convert(y, &j, why)) return false;
  *json_text = j.dump(2);
  return true;
}

Facts manifest_facts(const std::string& json_text, int* donor) {
  Facts f;
  *donor = -1;
  const json j = json::parse(json_text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) { f.donor_unknown = true; return f; }
  const json none = json::object();
  const json& functions = j.contains("functions") && j["functions"].is_object() ? j["functions"] : none;
  for (const auto& [key, value] : functions.items()) {
    if (!(value.is_number_integer() || value.is_number_unsigned())) continue;
    const uint64_t a = number(functions, key.c_str());
    if (address(a) && !retail_address(a)) ++f.functions_outside;
  }
  *donor = donor_of((uint32_t)number(functions, "onLoad"));
  f.donor_unknown = *donor < 0;
  if (j.contains("items") && j["items"].is_array()) f.items = (int)j["items"].size();
  // The move table: the pointer m-ex gives (a retail table, used as it is), else the rows a classic
  // package embeds, which must be its donor's (the clone path plays the donor's table).
  const uint64_t pointer = number(functions, "moveLogicPointer");
  const json& moves = j.contains("moveLogic") && j["moveLogic"].is_array() ? j["moveLogic"] : json::array();
  for (const auto& m : moves) {
    if (!m.is_object()) continue;
    for (const char* key : {"animationCallBack", "iASACallBack", "physicsCallback", "collisionCallback", "cameraCallback"}) {
      const uint64_t a = number(m, key);
      if (address(a) && !retail_address(a)) ++f.moves_outside;
    }
  }
  if (pointer == 0 && *donor >= 0) {
    const Donor& d = kDonors[*donor];
    if (moves.size() < d.rows) {
      f.own_move_table = !moves.empty();   // none at all: the donor's, as m-ex's own default
    } else {
      uint32_t h = 2166136261u;
      for (size_t i = 0; i < d.rows; ++i) {
        const json& m = moves[i].is_object() ? moves[i] : none;
        for (const char* key : {"animationID", "stateFlags", "attackID", "bitFlags", "animationCallBack", "iASACallBack",
                                "physicsCallback", "collisionCallback", "cameraCallback"})
          h = fnv_word(h, (uint32_t)number(m, key));
      }
      // Rows past the donor's are never entered by the game's own code: left as they are.
      f.own_move_table = h != d.hash;
    }
  }
  return f;
}

Result install(const fs::path& zip, const fs::path& fighters_folder, const std::string& root_hint, int mod_id,
               const std::string& title, const std::string& zip_sha256) {
  Result r;
  std::error_code ec;
  if (fighters_folder.empty()) { r.message = "Fighters are off in this launch."; return r; }
  const fs::path work = fs::temp_directory_path(ec) / L"NucleusFighters" / std::to_wstring(mod_id);
  fs::remove_all(work, ec);
  fs::create_directories(work / "package", ec);
  if (ec) { r.message = "A temporary folder cannot be made."; return r; }
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};
  std::string why;
  if (!host::archive::extract(zip, work / "package", kPackageLimits, &why)) {
    host::log("fighters: mod %d: the package could not be unpacked: %s", mod_id, why.c_str());
    r.message = "The package could not be unpacked.";
    return r;
  }
  const std::vector<Spot> spots = find_spots(work / "package");
  const Spot* spot = choose(spots, root_hint);
  if (!spot) { r.message = "This post holds no fighter package."; return r; }
  host::log("fighters: mod %d: %zu fighter%s in the package; installing the one at \"%s\"%s", mod_id, spots.size(),
            spots.size() == 1 ? "" : "s", spot->id().c_str(), !root_hint.empty() && spot->id() == root_hint ? " (the site's)" : "");
  fs::path base = work / "package" / fs::u8path(spot->folder);
  if (!spot->zip.empty()) {
    if (!host::archive::extract(work / "package" / fs::u8path(spot->zip), work / "inner", kPackageLimits, &why)) {
      host::log("fighters: mod %d: %s could not be unpacked: %s", mod_id, spot->zip.c_str(), why.c_str());
      r.message = "The package could not be unpacked.";
      return r;
    }
    base = work / "inner" / fs::u8path(spot->folder);
  }
  // fighter.json: as the package has it, or made from its fighter.yml.
  std::vector<uint8_t> bytes;
  const fs::path manifest = find_file(base, spot->yml ? "fighter.yml" : "fighter.json");
  if (manifest.empty() || !read_file(manifest, &bytes, 8u << 20)) { r.message = "The package's fighter cannot be read."; return r; }
  json j;
  if (spot->yml) {
    json y;
    if (!parse_yml(std::string(bytes.begin(), bytes.end()), &y, &why) || !convert(y, &j, &why)) {
      host::log("fighters: mod %d: %s", mod_id, why.c_str());
      r.message = "The package's fighter.yml cannot be read.";
      return r;
    }
  } else {
    size_t skip = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
    j = json::parse(bytes.begin() + (std::ptrdiff_t)skip, bytes.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) { r.message = "The package's fighter.json cannot be read."; return r; }
  }
  // Whether it plays here as it is: what fighter.json says, then its fighter file and folder.
  int donor = -1;
  Facts facts = manifest_facts(j.dump(), &donor);
  const json none = json::object();
  const json& files = j.contains("files") && j["files"].is_object() ? j["files"] : none;
  const fs::path data_file = find_file(base, str(files, "fighterDataPath"));
  std::vector<uint8_t> data;
  if (data_file.empty() || !read_file(data_file, &data, 256u << 20)) {
    r.message = "The package does not hold its fighter file.";
    return r;
  }
  facts.own_code_file = exports_code(data) == 1;
  if (fs::is_directory(base / "code", ec)) facts.code_folder = true;
  for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string low = lower(it->path().filename().u8string());
    for (const char* ext : {".c", ".h", ".mufighter"})
      if (low.size() > std::strlen(ext) && low.compare(low.size() - std::strlen(ext), std::strlen(ext), ext) == 0) facts.code_folder = true;
  }
  if (const std::string reason = needs_plugin(facts); !reason.empty()) {
    host::log("fighters: mod %d \"%s\": not installed: %s", mod_id, str(j, "name").c_str(), reason.c_str());
    r.needs_plugin = true;
    r.message = kNeedsPlugin;
    return r;
  }
  // What a classic package leaves to the disc it was made for: its move table (the donor's, which its
  // rows are) and its sound bank number (the donor's: the bank its sound ids name, and the one it keeps
  // when its own sounds cannot be used; those are kept in its sound folder, host/fighter_sound.h).
  const Donor& d = kDonors[donor];
  json& functions = j["functions"];
  if (!functions.is_object()) functions = json::object();
  if (number(functions, "moveLogicPointer") == 0) functions["moveLogicPointer"] = d.move_table;
  const uint64_t bank = number(j, "soundBank");
  if (!j.contains("soundBank") || bank >= kRetailBanks) {
    host::log("fighters: mod %d: sound bank %llu is not one of the game's: %s's (%u)", mod_id,
              (unsigned long long)(j.contains("soundBank") ? bank : kRetailBanks), d.name, d.bank);
    j["soundBank"] = d.bank;
  }
  j.erase("moveLogic");
  // Its Target Test stage (GrT<code>.dat): the roster carries no stage of a fighter's, so it is never
  // read here, and some packages ship a copy the game's loader would refuse. Left out.
  std::vector<fs::path> stages;
  for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string low = lower(it->path().filename().u8string());
    if (low.size() > 7 && low.rfind("grt", 0) == 0 && low.compare(low.size() - 4, 4, ".dat") == 0 && it->is_regular_file(ec))
      stages.push_back(it->path());
  }
  ec.clear();
  for (const auto& stage : stages) {
    fs::remove(stage, ec);
    host::log("fighters: mod %d: %s (its Target Test stage) left out", mod_id, stage.filename().u8string().c_str());
  }
  ec.clear();
  {
    std::ofstream out(base / "fighter.json", std::ios::binary | std::ios::trunc);
    const std::string text = j.dump(2);
    if (!out || !out.write(text.data(), (std::streamsize)text.size())) { r.message = "A temporary file cannot be written."; return r; }
  }
  // A costume's zip: the classic package names its portrait csp_<costume>.png and its stock icon
  // stock.png; the roster takes csp.png and stc.png.
  const auto unzip = [](const fs::path& costume_zip, const fs::path& dest) {
    std::string error;
    if (!host::archive::extract(costume_zip, dest, kCostumeLimits, &error)) return false;
    std::error_code e;
    fs::path csp, stc;
    bool has_csp = false, has_stc = false;
    for (fs::directory_iterator it(dest, e), end; !e && it != end; it.increment(e)) {
      const std::string low = lower(it->path().filename().u8string());
      if (low == "csp.png") has_csp = true;
      else if (low == "stc.png") has_stc = true;
      else if (low.rfind("csp", 0) == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".png") == 0) csp = it->path();
      else if (low == "stock.png" || (low.rfind("stc", 0) == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".png") == 0))
        stc = it->path();
    }
    if (!has_csp && !csp.empty()) fs::rename(csp, dest / "csp.png", e);
    if (!has_stc && !stc.empty()) fs::rename(stc, dest / "stc.png", e);
    return true;
  };
  host::roster::Installed done;
  std::string problem;
  fs::create_directories(fighters_folder, ec);
  if (!host::roster::install_package(base, fighters_folder, unzip, &done, &problem)) {
    host::log("fighters: mod %d: not installed: %s", mod_id, problem.c_str());
    r.message = problem;
    return r;
  }
  // Its own sound bank and name call, checked, beside its files (the game is served them at its next
  // start of a mode: host/fighter_sound.h).
  {
    std::string notes;
    const std::string sound = host::fighter_sound::take_package_sound(
        base, fighters_folder / fs::u8path(done.folder) / host::fighter_sound::kFolder, &notes);
    if (!sound.empty()) host::log("fighters: mod %d: its own sounds kept: %s", mod_id, sound.c_str());
    if (!notes.empty()) host::log("fighters: mod %d: %s; it uses %s's", mod_id, notes.c_str(), d.name);
  }
  const json origin = {{"mod_id", mod_id}, {"title", title}, {"donor", d.name}, {"sha256", zip_sha256},
                       {"root", spot->id()}, {"installed", now_text()}};
  {
    std::ofstream out(fighters_folder / fs::u8path(done.folder) / "nucleus.json", std::ios::binary | std::ios::trunc);
    out << origin.dump(2);
  }
  host::log("fighters: mod %d: installed \"%s\" as fighters\\%s, made from %s (sound bank %u, move table %08X)", mod_id,
            done.name.c_str(), done.folder.c_str(), d.name, (unsigned)number(j, "soundBank"),
            (unsigned)number(functions, "moveLogicPointer"));
  r.ok = true;
  r.folder = done.folder;
  r.name = done.name;
  r.donor = d.name;
  return r;
}

bool read_origin(const fs::path& fighter_folder, Origin* out) {
  std::vector<uint8_t> bytes;
  if (!read_file(fighter_folder / "nucleus.json", &bytes, 64u << 10)) return false;
  const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || number(j, "mod_id") == 0) return false;
  out->mod_id = (int)number(j, "mod_id");
  out->title = str(j, "title");
  out->donor = str(j, "donor");
  out->sha256 = str(j, "sha256");
  return true;
}

bool remove(const fs::path& fighters_folder, const std::string& folder, std::string* error) {
  Origin origin;
  if (fighters_folder.empty() || folder.empty() || folder.find_first_of("/\\:") != std::string::npos || folder[0] == '.') {
    *error = "No such fighter.";
    return false;
  }
  const fs::path dir = fighters_folder / fs::u8path(folder);
  if (!read_origin(dir, &origin)) {
    *error = "It was not installed from the Mods menu.";
    return false;
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (ec) {
    *error = "Its folder is in use. Try again.";
    return false;
  }
  host::log("fighters: removed fighters\\%s (mod %d)", folder.c_str(), origin.mod_id);
  return true;
}

}  // namespace host::fighter_package
