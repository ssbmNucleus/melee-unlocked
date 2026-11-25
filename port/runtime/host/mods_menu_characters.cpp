// The Mods menu's Characters (the category menu's first row): fighters' names and the roster's order,
// colours, Ice Climbers pairs.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {

const std::vector<std::string>& characters() { return host::mod_browser::fighters(); }

// A fighter's name as one key, whichever way the site spells it ("C. Falcon" and "Captain Falcon",
// "Mr. Game And Watch" and "Mr. Game & Watch"): the site's own table (docs/public-api.md, "Fighter
// names"), "&" read as "and".
std::string fighter_key(const std::string& name) {
  std::string k;
  for (size_t i = 0; i < name.size() && k.size() < 64; ++i) {
    const unsigned char c = (unsigned char)name[i];
    if (std::isalnum(c)) k += (char)std::tolower(c);
    else if (c == '&') k += "and";
  }
  static const std::map<std::string, std::string> aliases = {
      {"cfalcon", "captainfalcon"},   {"falcon", "captainfalcon"},       {"dk", "donkeykong"},
      {"captfalcon", "captainfalcon"}, {"mrgamewatch", "mrgameandwatch"}, {"gameandwatch", "mrgameandwatch"},
      {"gamewatch", "mrgameandwatch"}, {"gw", "mrgameandwatch"},          {"gandw", "mrgameandwatch"},
      {"gnw", "mrgameandwatch"},      {"doctormario", "drmario"},         {"doc", "drmario"},
      {"ic", "iceclimbers"},          {"ics", "iceclimbers"},             {"iceclimber", "iceclimbers"},
      {"iceclimbersnana", "iceclimbers"}, {"iceclimberspopo", "iceclimbers"}, {"popo", "iceclimbers"},
      {"nana", "iceclimbers"},        {"puff", "jigglypuff"},             {"jiggs", "jigglypuff"},
      {"jiggly", "jigglypuff"},       {"purin", "jigglypuff"},            {"koopa", "bowser"},
      {"ylink", "younglink"},         {"yl", "younglink"},                {"younglinkgirl", "younglink"},
      {"ganon", "ganondorf"},         {"shiek", "sheik"},                 {"seak", "sheik"},
      {"pika", "pikachu"},            {"m2", "mewtwo"},
  };
  const auto it = aliases.find(k);
  return it == aliases.end() ? k : it->second;
}

// The game's roster order (the character select screen, left to right, top row first): a pack's
// cards are listed in it.
int roster_index(const std::string& character) {
  static const char* const order[] = {"drmario", "mario", "luigi", "bowser", "peach", "yoshi", "donkeykong",
                                      "captainfalcon", "ganondorf", "falco", "fox", "ness", "iceclimbers",
                                      "kirby", "samus", "zelda", "sheik", "link", "younglink", "pichu",
                                      "pikachu", "jigglypuff", "mewtwo", "mrgameandwatch", "marth", "roy"};
  const std::string k = fighter_key(character);
  for (int i = 0; i < (int)(sizeof order / sizeof order[0]); ++i)
    if (k == order[i]) return i;
  return 100;
}

// Browse Characters' fighter lists in the game's roster order: "All fighters" first, then each
// fighter (its index in characters(), which is the site's alphabetical one).
const std::vector<int>& roster_filters() {
  static const std::vector<int> order = [] {
    std::vector<int> out;
    const auto& names = host::mod_browser::fighters();
    for (int i = 0; i < (int)names.size(); ++i) out.push_back(i);
    std::stable_sort(out.begin() + 1, out.end(),
                     [&](int a, int b) { return roster_index(names[(size_t)a]) < roster_index(names[(size_t)b]); });
    return out;
  }();
  return order;
}

int roster_row(int filter) {
  const auto& order = roster_filters();
  for (int i = 0; i < (int)order.size(); ++i)
    if (order[(size_t)i] == filter) return i;
  return 0;
}

// A fighter's name as a pack's rows show it, short enough to leave the colour its room.
std::string fighter_label(const std::string& name) {
  static const std::map<std::string, std::string> shorter = {
      {"captainfalcon", "C. Falcon"}, {"donkeykong", "DK"},          {"mrgameandwatch", "Game and Watch"},
      {"iceclimbers", "Ice Climbers"}, {"drmario", "Dr. Mario"},     {"younglink", "Young Link"},
  };
  const auto it = shorter.find(fighter_key(name));
  return it != shorter.end() ? it->second : name;
}

// The file's own name, without the folders some posts keep in it.
std::string base_of(const std::string& name) {
  const size_t slash = name.find_last_of("/\\");
  return slash == std::string::npos ? name : name.substr(slash + 1);
}

// The costume code of a character file's name: "PlCaBu.dat" -> "bu" ("" when the name is not one).
std::string colour_code(const Card& c) {
  const std::string name = lower(base_of(c.file_name));
  if (name.size() < 6 || name.compare(0, 2, "pl") != 0 || !std::isalpha((unsigned char)name[4]) ||
      !std::isalpha((unsigned char)name[5]))
    return {};
  return name.substr(4, 2);
}

// The game's costume codes and the colours they are, the default first, then the select screen's
// usual order.
struct Colour { const char* code; const char* word; };
constexpr Colour kColours[] = {{"nr", "Default"}, {"re", "Red"},     {"bu", "Blue"},      {"gr", "Green"},
                               {"ye", "Yellow"},  {"wh", "White"},   {"bk", "Black"},     {"or", "Orange"},
                               {"pi", "Pink"},    {"pu", "Purple"},  {"la", "Lavender"},  {"aq", "Aqua"},
                               {"cy", "Cyan"},    {"gy", "Grey"},    {"br", "Brown"}};
constexpr int kColourCount = (int)(sizeof kColours / sizeof kColours[0]);

// The colour slot a character file's own name says ("PlCaRe-GOKU.dat": Red); -1 when it says none.
// The game puts the file in that slot, whatever the site's `color` says.
int file_colour(const Card& c) {
  const std::string code = colour_code(c);
  for (int i = 0; i < kColourCount; ++i)
    if (code == kColours[i].code) return i;
  return -1;
}

// A card's colour as its row and the viewer say it: its file's own, else the site's; "" when only the
// site's is known and the site says it may be another file's (portrait_shared).
std::string colour_text(const Card& c) {
  const int own = file_colour(c);
  if (own >= 0) return kColours[own].word;
  return c.shares_portrait ? std::string() : c.color;
}

// A colour's place among a fighter's costumes (kColours' order): its file's own, else the site's word.
int colour_index(const Card& c) {
  const int own = file_colour(c);
  if (own >= 0) return own;
  const std::string colour = lower(c.color);
  if (colour == "neutral") return 0;
  if (colour == "gray") return 13;
  for (int i = 0; i < kColourCount; ++i)
    if (colour == lower(kColours[i].word)) return i;
  return 50;
}

// Ice Climbers costumes are two files, Popo's (PlPp...) and Nana's (PlNn...), and so two cards. On a
// page, Nana's card goes with Popo's of the same mod and slot: one row, and its install takes both.
int climber_of(const Card& c) {
  const std::string name = lower(base_of(c.file_name));
  return name.rfind("plpp", 0) == 0 ? 1 : name.rfind("plnn", 0) == 0 ? 2 : 0;
}
bool climbers_pair(const Card& popo, const Card& nana) {
  if (popo.mod_id != nana.mod_id || climber_of(popo) != 1 || climber_of(nana) != 2) return false;
  const std::string a = lower(base_of(popo.file_name)).substr(4), b = lower(base_of(nana.file_name)).substr(4);
  if (a == b) return true;
  return !popo.color.empty() && lower(popo.color) == lower(nana.color);
}
void pair_climbers(std::vector<Card>& cards) {
  for (size_t i = 0; i < cards.size();) {
    if (climber_of(cards[i]) == 2) {
      const auto popo = std::find_if(cards.begin(), cards.end(), [&](const Card& c) { return climbers_pair(c, cards[i]); });
      if (popo != cards.end() && popo->partners.empty()) {
        popo->partners.push_back(cards[i]);
        cards.erase(cards.begin() + (std::ptrdiff_t)i);
        continue;
      }
    }
    ++i;
  }
  for (auto& c : cards) c.multiple = cards.size() > 1;
}

// Manage's right column: the fighter, short enough to leave the name its room.
std::string short_fighter(const std::string& name) {
  static const std::map<std::string, std::string> shorter = {
      {"captainfalcon", "Falcon"}, {"donkeykong", "DK"},     {"iceclimbers", "ICs"},  {"mrgameandwatch", "G and W"},
      {"jigglypuff", "Puff"},      {"younglink", "Y. Link"}, {"ganondorf", "Ganon"},  {"drmario", "Dr. Mario"},
  };
  const auto it = shorter.find(fighter_key(name));
  return it != shorter.end() ? it->second : name;
}

// An Ice Climbers card's other half when its page did not hold it: from the mod's files on /mods/{id}.
std::vector<Card> climbers_partners(const Card& card) {
  std::vector<Card> out;
  const int half = climber_of(card);
  if (!half || !card.partners.empty() || card.mod_id <= 0) return out;
  json j;
  std::string error;
  int status = 0;
  if (!get_json(host::mod_browser::api_address() + "/mods/" + std::to_string(card.mod_id), &j, &status, &error) ||
      !j.is_object() || !j.contains("data") || !j["data"].is_object())
    return out;
  std::vector<Card> files;
  cards_of_post(j["data"], 0, std::string(), files);
  for (auto& f : files)
    if (f.file_id != card.file_id && (half == 1 ? climbers_pair(card, f) : climbers_pair(f, card))) {
      out.push_back(std::move(f));
      break;
    }
  return out;
}

}  // namespace menu_internal
}  // namespace host::mods_menu
