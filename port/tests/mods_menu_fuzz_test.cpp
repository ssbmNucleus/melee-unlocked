// The Mods menu's host half (host/mods_menu.cpp) fed what SSBM Nucleus might send in a /mods reply:
// every field of a post and of its files null, missing, the wrong type, empty, huge or odd Unicode
// (right-to-left marks, combining marks, emoji, NUL, lone surrogates, bytes that are not UTF-8), for
// the character list and the stage list, with and without a fighter filter. For each page it reads,
// it builds the list's replies, every row's viewer reply and picture reply, opens every pack, runs
// "Install all" (nothing is installed: the test is offline) and steps the pages, checking that every
// reply has its layout and that every word in it is one the game's font draws.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mod_browser.h"
#include "mods_menu.h"

namespace mm = host::mods_menu;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    if (g_failures < 40) std::printf("FAIL: %s %s\n", what, detail.c_str());
  }
}

uint32_t g_seed = 12345;
uint32_t rnd() {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 8;
}

// Odd values, as raw JSON text.
std::string huge(size_t n, char c) { return "\"" + std::string(n, c) + "\""; }
const std::vector<std::string>& odd_values() {
  static const std::vector<std::string> v = {
      "null", "true", "false", "0", "-1", "1.5", "9223372036854775807", "18446744073709551615",
      "99999999999999999999999", "-9223372036854775808", "1e308", "[]", "{}", "[1,2,{\"a\":null}]",
      "{\"deep\":{\"deeper\":[null]}}", "\"\"", "\" \"", huge(100000, 'x'), huge(5000, ' '),
      "\"\\u202e\\u200f evil \\u200b\\ufeff\"", "\"A\\u0301\\u0301\\u0301\\u0301\\u0301\"",
      "\"\\ud83d\\udca7 \\ud83c\\udfae emoji\"", "\"\\u0000NUL\\u0000\"", "\"\xe2\x80\xae raw RTL \xf0\x9f\x92\xa7\"",
      "\"<b>&amp;</b> %s %n %d {}\"", "\"line\\nbreak\\ttab\\r\"",
      "\"\\u3042\\u3044\\u3046 kana\"", "\"PlFxNr.dat\"", "\"Melee/PlKpDb.dat\"", "\"../../../etc/passwd\"",
      "\"C:\\\\Windows\\\\x.dat\"", "\"GrNBa.dat\"", "\"https://ssbmnucleus.net/x.png\"",
      "\"javascript:alert(1)\"", "\"http://127.0.0.1:1/x.png\"", "\"https://evil.example/x.png\""};
  return v;
}
std::string odd() { return odd_values()[rnd() % odd_values().size()]; }

std::string pick(const std::vector<std::string>& good) {
  // Mostly well-formed values, so the parser gets past the first field often enough.
  return rnd() % 3 ? good[rnd() % good.size()] : odd();
}

// One member of an object: "key": value, or nothing (the key missing).
void member(std::string& out, const char* key, const std::string& value) {
  if (rnd() % 9 == 0) return;
  if (out.back() != '{') out += ',';
  out += "\"";
  out += key;
  out += "\":";
  out += value;
}

std::string file_object(int category) {
  static const std::vector<std::string> names = {"\"PlFxNr.dat\"", "\"PlFxGr.dat\"", "\"Pack/Fox/PlFxBu.dat\"",
                                                 "\"PlPpNr.dat\"", "\"PlNnNr.dat\"", "\"PlCaBu.dat\"",
                                                 "\"MONO/Pl__.dat/Captain Falcon/PlCaGr.dat/PlCaGr.dat\"",
                                                 "\"GrNBa.dat\"", "\"GrPs.usd\"", "\"GrNLa Waffle.dat\""};
  static const std::vector<std::string> chars = {"\"Fox\"", "\"Captain Falcon\"", "\"C. Falcon\"", "\"Ice Climbers\"",
                                                 "\"Mr. Game & Watch\"", "\"Bowser\"", "null"};
  static const std::vector<std::string> colours = {"\"Default\"", "\"Red\"", "\"Blue\"", "\"Green\"", "null", "\"\""};
  static const std::vector<std::string> urls = {"\"https://media.ssbmnucleus.net/a/csp.png\"",
                                                "\"https://ssbmnucleus.net/api/posts/1/export-nucleus/2\"", "null"};
  if (rnd() % 15 == 0) return odd();   // not even an object
  std::string f = "{";
  member(f, "id", rnd() % 4 ? std::to_string(1 + rnd() % 100000) : odd());
  member(f, "filename", pick(names));
  member(f, "file_type", rnd() % 4 ? (category ? "\"stage_dat\"" : "\"character_dat\"") : odd());
  member(f, "character", pick(chars));
  member(f, "color", pick(colours));
  member(f, "slippi_safe", odd());
  member(f, "csp_url", pick(urls));
  member(f, "stock_url", pick(urls));
  member(f, "preview_url", pick(urls));
  member(f, "export_url", pick(urls));
  member(f, "download_url", pick(urls));
  member(f, "file_url", pick(urls));
  member(f, "size", rnd() % 2 ? std::to_string(rnd() % 1000000) : odd());
  member(f, "sha256", rnd() % 2 ? "\"" + std::string(64, 'a') + "\"" : odd());
  member(f, "unknown_field", odd());
  f += "}";
  return f;
}

std::string post_object(int category) {
  static const std::vector<std::string> titles = {"\"Monochrome Animelee\"", "\"Dry Bowser\"", "\"A\"", "\"\""};
  if (rnd() % 20 == 0) return odd();
  std::string p = "{";
  member(p, "id", rnd() % 4 ? std::to_string(1 + rnd() % 100000) : odd());
  member(p, "title", pick(titles));
  member(p, "description", odd());
  member(p, "author", odd());
  member(p, "creator", odd());
  member(p, "type", category ? "\"stage_skin\"" : "\"costume\"");
  member(p, "stage", odd());
  member(p, "tags", odd());
  member(p, "download_count", rnd() % 2 ? std::to_string(rnd() % 100000) : odd());
  member(p, "thumbnail_url", odd());
  member(p, "screenshots", rnd() % 2 ? "[\"https://media.ssbmnucleus.net/s.png\",null,5,[]]" : odd());
  if (rnd() % 8 == 0) {
    member(p, "files", odd());
  } else {
    std::string files = "[";
    const int n = (int)(rnd() % 7) + (rnd() % 10 == 0 ? 200 : 0);
    for (int i = 0; i < n; ++i) files += (i ? "," : "") + file_object(category);
    files += "]";
    member(p, "files", files);
  }
  p += "}";
  return p;
}

// Text the JSON reader refuses (a lone surrogate, bytes that are not UTF-8): the whole reply is then
// no list, so these go into a few replies only.
const char* const kPoison[] = {"\"\\ud800 lone\"", "\"\\udfff lone\"", "\"\xff\xfe not utf-8\"", "\"\xc3\""};

std::string body(int category) {
  switch (rnd() % 16) {
  case 0: return odd();
  case 1: return "{\"data\":" + odd() + "}";
  case 2: return "{\"data\":[" + odd() + "," + odd() + "]}";
  case 3: return "{\"data\":[{\"id\":1,\"files\":[{\"id\":2";   // cut short
  case 4: return "{\"data\":[{\"id\":1,\"title\":" + std::string(kPoison[rnd() % 4]) + "}]}";
  default: break;
  }
  std::string b = "{\"data\":[";
  const int n = (int)(rnd() % 60);
  for (int i = 0; i < n; ++i) b += (i ? "," : "") + post_object(category);
  b += "]";
  if (rnd() % 2) b += ",\"next_cursor\":" + (rnd() % 2 ? std::string("\"abc\"") : odd());
  if (rnd() % 2) b += ",\"total\":" + (rnd() % 2 ? std::to_string(rnd() % 5000) : odd());
  b += ",\"extra\":" + odd() + "}";
  return b;
}

// A /cards reply (docs/public-api.md, GET /cards with include_duplicates=1): cards of a few mods one
// after another, their fields as odd as a post's.
std::string card_object(int category, int mod_id) {
  static const std::vector<std::string> names = {"\"Files/PlMrSb.dat\"", "\"PlMrSc.dat\"", "\"PlFxNr.dat\"",
                                                 "\"PlPpNr.dat\"", "\"PlNnNr.dat\"", "\"GrNBa.dat\"", "\"GrPs.usd\""};
  static const std::vector<std::string> stage_files = {"\"GrNBa.dat\"", "\"GrPs.usd\"", "\"MnSlMap.usd\"", "null",
                                                       "\"../x.dat\""};
  static const std::vector<std::string> flags = {"true", "false", "null"};
  if (rnd() % 15 == 0) return odd();
  std::string c = "{";
  member(c, "id", rnd() % 4 ? std::to_string(1 + rnd() % 100000) : odd());
  member(c, "mod_id", rnd() % 6 ? std::to_string(mod_id) : odd());
  member(c, "mod_card_count", rnd() % 2 ? std::to_string(rnd() % 9) : odd());
  member(c, "mod_card_count_with_duplicates", rnd() % 2 ? std::to_string(rnd() % 12) : odd());
  member(c, "type", rnd() % 5 ? (category ? "\"stage_skin\"" : "\"costume\"") : odd());
  member(c, "title", rnd() % 2 ? "\"Sunshine Mario\"" : odd());
  member(c, "description", odd());
  member(c, "creator", odd());
  member(c, "filename", pick(names));
  member(c, "character", rnd() % 2 ? "\"Mario\"" : odd());
  member(c, "color", rnd() % 2 ? "\"Default\"" : odd());
  member(c, "stage", odd());
  member(c, "stage_file", pick(stage_files));
  member(c, "download_count", rnd() % 2 ? std::to_string(rnd() % 1000) : odd());
  member(c, "csp_url", rnd() % 2 ? "\"https://media.ssbmnucleus.net/p/csp.png\"" : odd());
  member(c, "portrait_shared", pick(flags));
  member(c, "hidden_duplicate", pick(flags));
  member(c, "export_url", odd());
  member(c, "download_url", rnd() % 2 ? "\"https://ssbmnucleus.net/api/public/v1/mods/1/download?file=2\"" : odd());
  member(c, "size", odd());
  member(c, "sha256", odd());
  c += "}";
  return c;
}

std::string cards_body(int category) {
  if (rnd() % 10 == 0) return body(category);
  std::string b = "{\"data\":[";
  const int mods = (int)(rnd() % 12);
  int k = 0;
  for (int m = 0; m < mods; ++m) {
    const int mod_id = 1 + (int)(rnd() % 50000), n = 1 + (int)(rnd() % 6) + (rnd() % 12 == 0 ? 120 : 0);
    for (int i = 0; i < n; ++i) b += (k++ ? "," : "") + card_object(category, mod_id);
  }
  b += "]";
  if (rnd() % 2) b += ",\"next_cursor\":" + (rnd() % 2 ? std::string("\"QDE\"") : odd());
  if (rnd() % 2) b += ",\"total\":" + (rnd() % 2 ? std::to_string(rnd() % 5000) : odd());
  b += "}";
  return b;
}

bool drawable_char(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
         c == ',' || c == '\'' || c == '"' || c == ':' || c == '-';
}

// A fixed-width text field: drawable characters, then NULs to its end.
bool field_ok(const std::vector<uint8_t>& r, size_t at, size_t width) {
  if (at + width > r.size()) return false;
  size_t i = at;
  while (i < at + width && r[i] != 0) {
    if (!drawable_char(r[i])) return false;
    ++i;
  }
  if (i == at + width) return false;   // no NUL: the last byte must end it
  for (; i < at + width; ++i)
    if (r[i] != 0) return false;
  return true;
}

constexpr size_t kHead = 1 + 2 + 2 + 2 + 1 + 20 + 48 + 48;
constexpr size_t kRow = 40 + 12 + 1;
constexpr size_t kFileId = 6 + 5 * 48 + 400;        // ... description, then the card's file id
constexpr size_t kDetail = kFileId + 4 + 1;          // ... and 1 when A asks before it replaces a menu mod

int check_list(const char* where) {
  std::vector<uint8_t> r;
  mm::list(0, 60, r);
  check(r.size() >= kHead, where, "list head");
  if (r.size() < kHead) return 0;
  const int rows = r[1] << 8 | r[2];
  const int shown = rows < 60 ? rows : 60;
  check(r[0] <= 2, where, "list status");
  check(r.size() == kHead + (size_t)shown * kRow, where, "list size " + std::to_string(r.size()) + " rows " + std::to_string(rows));
  check(field_ok(r, 8, 20) && field_ok(r, 28, 48) && field_ok(r, 76, 48), where, "list head words");
  for (int i = 0; i < shown && kHead + (size_t)(i + 1) * kRow <= r.size(); ++i) {
    const size_t at = kHead + (size_t)i * kRow;
    check(field_ok(r, at, 40) && field_ok(r, at + 40, 12), where, "row words " + std::to_string(i));
    const uint8_t state = r[at + 52];
    check((state & 0x3F) <= 5 && (state & 0xC0) != 0xC0, where, "row state");
  }
  // Rows past the end: no rows, no crash.
  mm::list(rows + 5, 60, r);
  mm::list(-3, 200, r);
  return rows;
}

void check_rows(int rows, const char* where) {
  for (int row = -2; row < rows + 2; ++row) {
    std::vector<uint8_t> r;
    mm::detail(row, r);
    check(r.size() == 1 ? r[0] == 0xFF : r.size() == kDetail, where, "detail size " + std::to_string(r.size()));
    if (r.size() == kDetail) {
      bool words = true;
      for (int k = 0; k < 5; ++k) words = words && field_ok(r, 6 + 48 * (size_t)k, 48);
      words = words && field_ok(r, 6 + 5 * 48, 400);
      check(words, where, "detail words");
      check(r[kDetail - 1] == 0, where, "no menu mod to replace");   // these lists have no menu screens
    }
    mm::picture(row, r);
    check(!r.empty(), where, "picture reply");
  }
}

int g_pages_read = 0, g_rows_seen = 0, g_packs_opened = 0;

void walk_page(const char* where) {
  const int rows = check_list(where);
  g_rows_seen += rows;
  check_rows(rows, where);
  // Every pack: its list, its rows, "Install all", the pages, and back.
  for (int row = 0; row < rows; ++row) {
    if (!mm::change(5, row)) continue;
    ++g_packs_opened;
    const int in_pack = check_list(where);
    check_rows(in_pack, where);
    mm::change(7);
    check_list(where);
    while (mm::change(1)) check_rows(check_list(where), where);
    while (mm::change(0)) {}
    check(mm::change(6), where, "back from a pack");
  }
  mm::change(1);
  mm::change(0);
  mm::change(2);   // the fighter: only in Browse Characters with no pack open
  mm::change(3);
  mm::change(4);
  mm::change(-1);
  mm::change(99, 70000);
}

void drawable_fuzz() {
  for (int k = 0; k < 2000; ++k) {
    std::string s;
    const size_t n = rnd() % 300;
    for (size_t i = 0; i < n; ++i) s += (char)(rnd() & 0xFF);
    const size_t max = rnd() % 60;
    const std::string d = mm::drawable(s, max);
    bool ok = d.size() <= max;
    for (const char c : d) ok = ok && drawable_char((uint8_t)c);
    check(ok, "drawable", s.substr(0, 20));
  }
  // Letters with marks as the plain letters, curly quotes and dashes as straight ones; the rest left out.
  check(mm::drawable("Pok\xC3\xA9mon Stadium", 60) == "Pokemon Stadium", "drawable", "Pokemon with its accent");
  check(mm::drawable("\xC3\x9C" "ber Se\xC3\xB1or", 60) == "Uber Senor", "drawable", "Uber Senor");
  check(mm::drawable("\xE2\x80\x9CMario\xE2\x80\x9D \xE2\x80\x93 Luigi\xE2\x80\x99s", 60) == "\"Mario\" - Luigi's", "drawable", "curly marks");
  check(mm::drawable("A\xC3\x97" "B \xF0\x9F\x92\xA7 C", 60) == "AB C", "drawable", "a sign and an emoji left out");
  check(mm::drawable("x\xC3", 60) == "x" && mm::drawable("\xC3" "A", 60) == "A", "drawable", "a broken sequence left out");
}

std::string read_file(const char* path) {
  std::string out;
  if (FILE* f = path ? std::fopen(path, "rb") : nullptr) {
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
  }
  return out;
}

std::string text_at(const std::vector<uint8_t>& r, size_t at) {
  return at < r.size() ? std::string((const char*)&r[at]) : std::string();
}
uint32_t u32_at(const std::vector<uint8_t>& r, size_t at) {
  return at + 4 <= r.size() ? (uint32_t)r[at] << 24 | (uint32_t)r[at + 1] << 16 | (uint32_t)r[at + 2] << 8 | r[at + 3] : 0;
}

// The real site's "DBZ Pack | Goku" (mod 16421), as /cards?mod_id=16421&include_duplicates=1 answered
// (tests/fixtures/nucleus_cards_16421.json): twelve Captain Falcon files, a "-HALO" one and a plain one
// for each of six colours, every card portrait_shared, each with its own portrait and stock icon.
// Every row must open (and install) the card its label names; no two labels the same.
struct GokuCard { uint32_t id; std::string file, colour; bool halo; };

std::vector<GokuCard> goku_cards(const std::string& reply) {
  // (a small reader for this fixture: its "id", "filename" and "color" in order, one card per "mod_id")
  std::vector<GokuCard> out;
  size_t at = 0;
  const auto field = [&](const char* key, size_t from) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = reply.find(k, from);
    if (p == std::string::npos) return std::string();
    size_t v = p + k.size();
    if (reply[v] == '"') {
      const size_t e = reply.find('"', v + 1);
      return reply.substr(v + 1, e - v - 1);
    }
    size_t e = v;
    while (e < reply.size() && reply[e] != ',' && reply[e] != '}') ++e;
    return reply.substr(v, e - v);
  };
  while ((at = reply.find("{\"id\":", at)) != std::string::npos) {
    GokuCard c;
    c.id = (uint32_t)std::strtoul(field("id", at).c_str(), nullptr, 10);
    c.file = field("filename", at);
    c.colour = field("color", at);
    c.halo = c.file.find("-HALO") != std::string::npos;
    out.push_back(c);
    at += 6;
  }
  return out;
}

void goku_mapping(const std::vector<GokuCard>& cards, const char* when) {
  static const char* const words[] = {"Default", "Red", "Blue", "Green", "White", "Grey"};
  static const char* const codes[] = {"Nr", "Re", "Bu", "Gr", "Wh", "Gy"};
  std::vector<uint8_t> r;
  mm::list(0, 60, r);
  const int rows = r.size() >= kHead ? (r[1] << 8 | r[2]) : 0;
  check(rows == 13, when, "Install all and twelve rows: " + std::to_string(rows));
  std::vector<std::string> labels;
  for (int i = 1; i < rows && i <= 12; ++i) {
    const std::string label = text_at(r, kHead + (size_t)i * kRow);
    labels.push_back(label);
    // The rows in roster and colour order, the plain file before its HALO one.
    const int k = (i - 1) / 2;
    const bool halo = (i - 1) % 2 == 1;
    const std::string want = std::string("C. Falcon - ") + words[k] + (halo ? " - HALO" : "");
    check(label == want, when, "row " + std::to_string(i) + " label " + label + " (want " + want + ")");
    // Its viewer: the card whose file is that colour's, HALO or not.
    const GokuCard* card = nullptr;
    for (const auto& c : cards)
      if (c.file.compare(0, 6, std::string("PlCa") + codes[k]) == 0 && c.halo == halo) card = &c;
    check(card != nullptr, when, "a card for " + want);
    if (!card) continue;
    std::vector<uint8_t> d;
    mm::detail(i, d);
    check(d.size() == kDetail && u32_at(d, kFileId) == card->id, when,
          want + ": opens card " + std::to_string(u32_at(d, kFileId)) + ", want " + std::to_string(card->id) + " " + card->file);
    check(text_at(d, 6) == want, when, "viewer name " + text_at(d, 6));
    const std::string stem = card->file.substr(0, card->file.size() - 4);
    check(text_at(d, 54) == "File: " + stem, when, "viewer file line " + text_at(d, 54));
    check(text_at(d, 6 + 5 * 48).rfind("Picture may differ.", 0) != 0, when, want + ": no 'picture may differ' (its own portrait)");
    check(mm::withheld_for_test(i) == 0, when, want + ": its own portrait and stock icon kept");
    const std::string name = mm::install_name_for_test(i);
    check(name == std::string("DBZ Pack Goku - ") + words[k] + (halo ? " - HALO" : ""), when, "install name " + name);
    // A refuses another card's id, takes this one's (offline: queued, never run).
    check(!mm::action(i, (int)card->id + 1), when, want + ": A with another card's id refused");
  }
  for (size_t a = 0; a < labels.size(); ++a)
    for (size_t b = a + 1; b < labels.size(); ++b) check(labels[a] != labels[b], when, "two rows read " + labels[a]);
}

void goku_pack(const char* path) {
  const std::string reply = read_file(path);
  check(!reply.empty(), "Goku fixture", path ? path : "(no path given)");
  if (reply.empty()) return;
  const auto cards = goku_cards(reply);
  check(cards.size() == 12, "Goku fixture cards", std::to_string(cards.size()));
  // Its row as "All fighters" shows it: a post of /mods with its six visible (HALO) files.
  std::string post = "{\"data\":[{\"id\":16421,\"title\":\"DBZ Pack | Goku\",\"creator\":\"Vancity_Primal\",\"download_count\":196,"
                     "\"type\":\"costume\",\"files\":[";
  int n = 0;
  for (const auto& c : cards) {
    if (!c.halo) continue;
    post += std::string(n++ ? "," : "") + "{\"id\":" + std::to_string(c.id) + ",\"filename\":\"" + c.file +
            "\",\"file_type\":\"character_dat\",\"character\":\"Captain Falcon\",\"color\":\"" + c.colour +
            "\",\"csp_url\":\"https://media.ssbmnucleus.net/posts/2023/03/post_16421/" + c.file.substr(0, c.file.size() - 4) +
            "_GENERATED_csp.png\",\"export_url\":\"https://ssbmnucleus.net/api/posts/16421/export-nucleus/" +
            std::to_string(c.id) + "\"}";
  }
  post += "]}],\"next_cursor\":null,\"total\":1}";
  check(mm::load_browse_page_for_test(0, 0, post), "Goku post page");
  std::vector<uint8_t> r;
  mm::list(0, 60, r);
  check(r.size() == kHead + kRow && text_at(r, kHead + 40) == "6 skins", "Goku: one pack row of 6 (the site's visible files)",
        text_at(r, kHead + 40));
  check(mm::change(5, 0), "open Goku");
  // Before its own list comes: the row's six cards. The viewer of row 3 then; the list comes, and A on
  // row 3 with that card's id is refused if row 3 is another card now.
  std::vector<uint8_t> before;
  mm::detail(3, before);
  const uint32_t seen = u32_at(before, kFileId);
  // Its own list: the six plain files are hidden on the site as duplicates of the HALO ones' portraits,
  // and stay hidden here (the owner's choice): "Install all" and the six HALO files, each row opening
  // the HALO card of its colour.
  check(mm::load_pack_for_test(16421, reply), "Goku's own list");
  {
    static const char* const codes[] = {"Nr", "Re", "Bu", "Gr", "Wh", "Gy"};
    mm::list(0, 60, r);
    const int rows = r.size() >= kHead ? (r[1] << 8 | r[2]) : 0;
    check(rows == 7, "Goku: Install all and the six it shows", std::to_string(rows));
    std::vector<std::string> labels;
    for (int i = 1; i < rows && i <= 6; ++i) {
      labels.push_back(text_at(r, kHead + (size_t)i * kRow));
      std::vector<uint8_t> d;
      mm::detail(i, d);
      const uint32_t id = u32_at(d, kFileId);
      const GokuCard* card = nullptr;
      for (const auto& c : cards)
        if (c.id == id) card = &c;
      check(card != nullptr && card->halo, "Goku: a row opens a HALO card", labels.back());
      check(card != nullptr && card->file.compare(0, 6, std::string("PlCa") + codes[i - 1]) == 0,
            "Goku: rows in colour order", labels.back() + " " + (card ? card->file : std::string()));
    }
    for (size_t a = 0; a < labels.size(); ++a)
      for (size_t b = a + 1; b < labels.size(); ++b) check(labels[a] != labels[b], "Goku: two rows read", labels[a]);
  }
  std::vector<uint8_t> after;
  mm::detail(3, after);
  if (u32_at(after, kFileId) != seen) check(!mm::action(3, (int)seen), "Goku: A after the list changed under the viewer refused");
  mm::list(0, 60, r);
  check(text_at(r, 28).rfind("6 skins", 0) == 0, "Goku: 6 skins once its list is read", text_at(r, 28));
  check(mm::change(6), "back from Goku");
  mm::list(0, 60, r);
  check(r.size() == kHead + kRow && text_at(r, kHead + 40) == "6 skins", "Goku's row: the six the site shows",
        text_at(r, kHead + 40));
  // The same twelve files all shown by the site (as they are while kListHiddenDuplicates): every row
  // opens the card its label names, before and after B.
  std::string all = reply;
  for (const char* hidden : {"\"hidden_duplicate\":true", "\"hidden_duplicate\": true"}) {
    const std::string from = hidden, to = from.substr(0, from.size() - 4) + "false";
    for (size_t at = all.find(from); at != std::string::npos; at = all.find(from, at + to.size())) all.replace(at, from.size(), to);
  }
  check(mm::load_pack_for_test(16421, all), "Goku's twelve");
  check(mm::change(5, 0), "open Goku, twelve shown");
  goku_mapping(cards, "Goku, twelve shown");
  check(mm::change(6), "back from Goku, twelve shown");
  check(mm::change(5, 0), "open Goku again");
  goku_mapping(cards, "Goku after B and back");
  check(mm::change(6), "back from Goku again");
}

// The real site's first page of /mods (type=costume, sort=downloads, limit=10;
// tests/fixtures/nucleus_mods_costume_10.json): ten posts, ten rows, the 113-skin Monochrome Animelee
// one row of them.
void all_fighters_first_screen(const char* path) {
  const std::string reply = read_file(path);
  check(!reply.empty(), "/mods fixture", path ? path : "(no path given)");
  if (reply.empty()) return;
  check(mm::load_browse_page_for_test(0, 0, reply), "/mods first page");
  std::vector<uint8_t> r;
  mm::list(0, 60, r);
  const int rows = r.size() >= kHead ? (r[1] << 8 | r[2]) : 0;
  check(rows == 10, "ten posts, ten rows", std::to_string(rows));
  check(rows > 0 && text_at(r, kHead) == "Monochrome Animelee" && text_at(r, kHead + 40) == "113 skins",
        "Monochrome Animelee one row of 113", text_at(r, kHead) + " / " + text_at(r, kHead + 40));
  std::string titles;
  for (int i = 0; i < rows; ++i) titles += text_at(r, kHead + (size_t)i * kRow) + ";";
  std::printf("mods menu: All fighters, first screen: %s\n", titles.c_str());
  check((r[7] & 2) != 0, "more after it");
  check(text_at(r, 28) == "3,373 mods", "the bar's count: the site's posts", text_at(r, 28));
  // One continuous list: A at its end asks for the next batch once ("Loading..." meanwhile), and the
  // batch, once in, is the same list ten rows longer with every row before where it was.
  check(mm::change(1), "more asked for");
  check(!mm::change(1), "more not asked for twice while on its way");
  mm::list(0, 60, r);
  check(r.size() >= kHead && (r[7] & 64) != 0, "more on its way");
  const std::string first = text_at(r, kHead);
  std::string batch = "{\"data\":[";
  for (int i = 0; i < 10; ++i)
    batch += std::string(i ? "," : "") + "{\"id\":" + std::to_string(900001 + i) + ",\"title\":\"Batch two " +
             std::to_string(i) + "\",\"download_count\":5,\"files\":[{\"id\":" + std::to_string(910001 + i) +
             ",\"filename\":\"PlFxNr.dat\",\"file_type\":\"character_dat\",\"character\":\"Fox\",\"color\":\"Default\"}]}";
  batch += "],\"next_cursor\":null,\"total\":20}";
  check(mm::load_more_for_test(batch), "a second batch");
  mm::list(0, 60, r);
  const int longer = r.size() >= kHead ? (r[1] << 8 | r[2]) : 0;
  check(longer == 20 && text_at(r, kHead) == first && (r[7] & 64) == 0, "twenty rows, the first where it was",
        std::to_string(longer));
  check((r[7] & 2) == 0, "the true end: nothing more after it");
  std::vector<uint8_t> tail;
  mm::list(10, 10, tail);
  check(tail.size() >= kHead + kRow && text_at(tail, kHead) == "Batch two 0", "the batch's rows after the first ten",
        tail.size() >= kHead + kRow ? text_at(tail, kHead) : std::string());
  check(!mm::change(1), "no more after the true end");
}

// Browse Characters opens on the fighters: "All fighters", then each fighter in the game's roster order;
// A on one is its posts, B there back to the fighters, on it; the list opens next on the one chosen last.
void fighters_first() {
  std::vector<uint8_t> r;
  mm::open(0, 0);
  mm::list(0, 60, r);
  const int rows = r.size() >= kHead ? (r[1] << 8 | r[2]) : 0;
  check(rows == 27 && (r[7] & 1) != 0, "the fighters' list first", std::to_string(rows));
  check(r.size() >= kHead + 3 * kRow && text_at(r, kHead) == "All fighters" && text_at(r, kHead + kRow) == "Dr. Mario" &&
            text_at(r, kHead + 2 * kRow) == "Mario",
        "the roster's order", r.size() >= kHead + 2 * kRow ? text_at(r, kHead + kRow) : std::string());
  check((r[3] << 8 | r[4]) == 0, "on All fighters at first");
  // The right column: every row's count or none (the site's /cards/counts, one request for all).
  const auto counts_shown = [&](const std::vector<uint8_t>& reply) {
    int shown = 0;
    for (size_t i = 0; i < 27 && reply.size() >= kHead + (i + 1) * kRow; ++i) shown += text_at(reply, kHead + i * kRow + 40).empty() ? 0 : 1;
    return shown;
  };
  check(counts_shown(r) == 0, "no counts before they are in", std::to_string(counts_shown(r)));
  check(!mm::load_counts_for_test(0, 404, "{\"error\":{\"code\":\"not_found\",\"message\":\"No such endpoint.\"}}"),
        "a site without counts");
  check(!mm::load_counts_for_test(0, 200, "{\"data\":{\"Mario\":-1},\"total\":3}"), "a count that is not one");
  mm::list(0, 60, r);
  check(counts_shown(r) == 0, "none when the site has none", std::to_string(counts_shown(r)));
  check(mm::load_counts_for_test(0, 200, "{\"data\":{\"Mario\":12,\"C. Falcon\":2,\"Captain Falcon\":3,"
                                         "\"Mr. Game And Watch\":1,\"wolf\":4},\"total\":12345}"),
        "counts read");
  mm::list(0, 60, r);
  check(counts_shown(r) == 27, "every row's count at once", std::to_string(counts_shown(r)));
  check(text_at(r, kHead + 40) == "12345 skins" && text_at(r, kHead + kRow + 40) == "0 skins" &&
            text_at(r, kHead + 2 * kRow + 40) == "12 skins" && text_at(r, kHead + 8 * kRow + 40) == "5 skins" &&
            text_at(r, kHead + 24 * kRow + 40) == "1 skin",
        "the counts: all, none, Mario, C. Falcon's spellings added, Game and Watch",
        text_at(r, kHead + 8 * kRow) + " " + text_at(r, kHead + 8 * kRow + 40) + ", " + text_at(r, kHead + 24 * kRow) + " " +
            text_at(r, kHead + 24 * kRow + 40));
  check(!mm::change(1) && !mm::change(3), "nothing else changes the fighters' list");
  check(mm::change(9, 2), "Mario chosen");
  mm::list(0, 60, r);
  check(r.size() >= kHead && (r[7] & 1) == 0 && text_at(r, 8) == "Mario", "Mario's posts", text_at(r, 8));
  check(mm::change(3), "R: the next fighter");
  mm::list(0, 60, r);
  check(r.size() >= kHead && text_at(r, 8) == "Luigi", "R after Mario is Luigi", text_at(r, 8));
  check(mm::change(10), "B: back to the fighters");
  mm::list(0, 60, r);
  check(r.size() >= kHead && (r[7] & 1) != 0 && (r[3] << 8 | r[4]) == 3, "back on Luigi", std::to_string(r[3] << 8 | r[4]));
  mm::open(0, 1);
  mm::list(0, 60, r);
  // Stages open on the six legal stages instead, in the stage select's order (host/mods_menu_stages.cpp).
  check(r.size() >= kHead + kRow && (r[7] & 1) != 0 && (r[1] << 8 | r[2]) == 6 && text_at(r, kHead) == "Yoshi's Story",
        "Stages: the six stages, not the fighters", r.size() >= kHead + kRow ? text_at(r, kHead) : std::string());
  mm::open(0, 0);
  mm::list(0, 60, r);
  check(r.size() >= kHead && (r[3] << 8 | r[4]) == 3, "opened again on the fighter chosen last");
  mm::change(9, 0);
}

}  // namespace

int main(int argc, char** argv) {
  mm::set_offline_for_test(true);
  goku_pack(argc > 1 ? argv[1] : nullptr);
  all_fighters_first_screen(argc > 2 ? argv[2] : nullptr);
  fighters_first();
  drawable_fuzz();
  // Pictures from tiny to too large: the archive or nothing, never a crash.
  for (const int side : {0, 4, 5, 8, 256, 300}) {
    const std::vector<uint8_t> px((size_t)side * side * 4, 0x80);
    const auto a = mm::picture_archive(px, side, side);
    check(side % 4 == 0 && side > 0 ? !a.empty() : a.empty(), "picture_archive", std::to_string(side));
  }
  check(mm::picture_archive(std::vector<uint8_t>(10), 4, 4).empty(), "picture_archive short pixels");
  const auto wide = mm::picture_archive(std::vector<uint8_t>(64 * 16 * 4, 1), 64, 16);
  check(!wide.empty(), "picture_archive wide");
  for (int round = 0; round < 400; ++round) {
    const int category = round % 2;
    const int filter = round % 5 == 0 ? (int)(rnd() % 30) : 0;
    const std::string b = round % 3 == 2 ? cards_body(category) : body(category);
    g_pages_read += mm::load_browse_page_for_test(category, filter, b) ? 1 : 0;
    walk_page(category ? "stages" : "characters");
  }
  // The odd replies must not all have been refused whole: most were read, with rows and packs in them.
  std::printf("mods menu fuzz: %d of 400 replies read as lists, %d rows, %d packs opened\n", g_pages_read, g_rows_seen,
              g_packs_opened);
  check(g_pages_read >= 200 && g_rows_seen >= 1000 && g_packs_opened >= 100, "coverage of the fuzz");
  // A well-formed pack, so the paths above are certain to have run at least once.
  std::string pack = "{\"data\":[{\"id\":7,\"title\":\"Monochrome Animelee\",\"creator\":\"\",\"author\":\"x\","
                     "\"download_count\":1539,\"files\":[";
  for (int i = 0; i < 120; ++i)
    pack += std::string(i ? "," : "") + "{\"id\":" + std::to_string(100 + i) +
            ",\"filename\":\"MONO/Pl__.dat/Captain Falcon/PlCa" + (i % 2 ? "Bu" : "Gr") + ".dat/v" + std::to_string(i) +
            "/PlCa.dat\",\"file_type\":\"character_dat\",\"character\":\"Captain Falcon\",\"color\":\"" +
            (i % 2 ? "Blue" : "Green") + "\"}";
  pack += "]}],\"next_cursor\":null,\"total\":1}";
  check(mm::load_browse_page_for_test(0, 0, pack), "pack page");
  check(check_list("pack") == 1, "one row for the pack");
  check(mm::change(5, 0), "open the pack");
  {
    std::vector<uint8_t> r;
    mm::list(0, 60, r);
    check(r.size() > kHead && (r[7] & 8) != 0, "in the pack");
    // The first row is "Install all"; then cards labelled by fighter and colour, not by the title.
    check(r.size() >= kHead + 2 * kRow && (r[kHead + 52] & 0x80) != 0, "install all row");
    const std::string first((const char*)&r[kHead + kRow]);
    check(first.rfind("C. Falcon - ", 0) == 0, "card label", first);
    check(first.find("Monochrome") == std::string::npos, "label without the pack title", first);
  }
  {
    // A pack's list is whole: "Install all" and its 120 cards, no page after it.
    std::vector<uint8_t> r;
    mm::list(0, 60, r);
    const int n = r.size() >= kHead ? (r[1] << 8 | r[2]) : 0;
    check(n == 121 && (r[7] & 2) == 0, "the pack's whole list", std::to_string(n));
    check(!mm::change(1), "no second page in a pack");
  }
  check(mm::change(6), "back to the posts");
  // The real site's "Sunshine Mario": five Mario files recorded as "Default" with one portrait, four
  // hidden as duplicates (no download_url). The hidden cards stay hidden (the owner's choice), even
  // should the site send them: one card, no pack. The same five shown by the site: one row, a pack of 5;
  // its rows told apart by their files' names; the viewer says the picture may differ.
  {
    const auto sunshine = [](bool hide) {
      std::string sun = "{\"data\":[";
      const char* const stems[] = {"PlMrSb", "PlMrSc", "PlMrSg", "PlMrSs", "PlMrSy"};
      for (int i = 0; i < 5; ++i)
        sun += std::string(i ? "," : "") + "{\"id\":" + std::to_string(12186 + i) + ",\"mod_id\":16086,\"mod_card_count\":" +
               (hide ? "1" : "5") + ",\"mod_card_count_with_duplicates\":5,\"type\":\"costume\","
               "\"title\":\"Sunshine Mario\",\"creator\":\"kthnrssbb33\",\"author\":\"SSBM Textures\",\"filename\":\"Files/" +
               stems[i] + ".dat\",\"character\":\"Mario\",\"color\":\"Default\",\"download_count\":243,"
               "\"csp_url\":\"https://media.ssbmnucleus.net/posts/2022/10/post_16086/UI/csp.png\",\"portrait_shared\":true,"
               "\"hidden_duplicate\":" + (hide && i ? "true" : "false") +
               ",\"export_url\":\"https://ssbmnucleus.net/api/posts/16086/export-nucleus/" + std::to_string(12186 + i) +
               "\",\"download_url\":\"https://ssbmnucleus.net/api/public/v1/mods/16086/download?file=" +
               std::to_string(12186 + i) + "\"}";
      return sun + "],\"next_cursor\":null,\"total\":5}";
    };
    std::vector<uint8_t> r;
    check(mm::load_browse_page_for_test(0, 0, sunshine(true)), "cards page, four hidden");
    mm::list(0, 60, r);
    check(r.size() == kHead + kRow && (r[kHead + 52] & 0x40) == 0, "Sunshine Mario with four hidden: one card");
    check(!mm::change(5, 0), "no pack to open");
    check(mm::load_browse_page_for_test(0, 0, sunshine(false)), "cards page");
    mm::list(0, 60, r);
    check(r.size() == kHead + kRow && (r[kHead + 52] & 0x40) != 0, "one pack row for Sunshine Mario");
    check(r.size() >= kHead + kRow && std::string((const char*)&r[kHead + 40]) == "5 skins", "Sunshine Mario: 5 skins",
          r.size() >= kHead + kRow ? std::string((const char*)&r[kHead + 40]) : std::string());
    check(mm::change(5, 0), "open Sunshine Mario");
    mm::list(0, 60, r);
    check(r.size() == kHead + 6 * kRow, "install all and five rows", std::to_string(r.size()));
    std::string labels;
    for (int i = 1; i < 6 && kHead + (size_t)(i + 1) * kRow <= r.size(); ++i)
      labels += std::string((const char*)&r[kHead + (size_t)i * kRow]) + ";";
    check(labels == "PlMrSb;PlMrSc;PlMrSg;PlMrSs;PlMrSy;", "rows by file name", labels);
    mm::detail(2, r);
    check(r.size() == kDetail && std::string((const char*)&r[6 + 5 * 48]).rfind("Picture may differ.", 0) == 0,
          "the viewer's note", r.size() == kDetail ? std::string((const char*)&r[6 + 5 * 48]).substr(0, 40) : "");
    check(r.size() == kDetail && std::string((const char*)&r[6 + 48]) == "Mario - PlMrSc", "the viewer's kind line",
          r.size() == kDetail ? std::string((const char*)&r[6 + 48]) : "");
    check(mm::change(6), "back from Sunshine Mario");
    // A post at the page's end with one card here and more on the next page: a pack by the site's count.
    std::string edge = "{\"data\":[{\"id\":1,\"mod_id\":5,\"mod_card_count\":1,\"type\":\"costume\",\"title\":\"One\","
                       "\"filename\":\"PlFxNr.dat\",\"character\":\"Fox\",\"color\":\"Default\",\"download_url\":\"https://"
                       "ssbmnucleus.net/x\"},{\"id\":2,\"mod_id\":6,\"mod_card_count\":4,\"mod_card_count_with_duplicates\":7,"
                       "\"type\":\"costume\",\"title\":\"Split\",\"filename\":\"PlFxRe.dat\",\"character\":\"Fox\","
                       "\"color\":\"Red\",\"download_url\":\"https://ssbmnucleus.net/y\"}],\"next_cursor\":\"QDE\",\"total\":9}";
    check(mm::load_browse_page_for_test(0, 0, edge), "edge page");
    mm::list(0, 60, r);
    check(r.size() == kHead + 2 * kRow && (r[kHead + 52] & 0x40) == 0 && (r[kHead + kRow + 52] & 0x40) != 0,
          "a lone card, then a pack going on over the next page");
    // (the site's count of the cards it shows: the hidden duplicates are no skins here)
    check(r.size() == kHead + 2 * kRow && std::string((const char*)&r[kHead + kRow + 40]) == "4 skins", "its count",
          r.size() == kHead + 2 * kRow ? std::string((const char*)&r[kHead + kRow + 40]) : std::string());
    // The search: its words in the head, and a flag; "" ends it.
    check(mm::search("sun"), "search");
    mm::list(0, 60, r);
    check(r.size() >= kHead && (r[7] & 32) != 0 && std::string((const char*)&r[8]) == "\"sun\"", "search words in the head",
          r.size() >= kHead ? std::string((const char*)&r[8]) : "");
    check(!mm::search("sun"), "the same search again");
    // Back from the name entry (open again): the search stays; a fresh open ends it.
    mm::open(0, 0, true);
    mm::list(0, 60, r);
    check(r.size() >= kHead && (r[7] & 32) != 0, "the search kept when the list opens again");
    mm::open(0, 0);
    mm::list(0, 60, r);
    check(r.size() >= kHead && (r[7] & 32) == 0, "a fresh open ends the search");
    check(mm::search("sun"), "search again");
    check(mm::search(std::string("\x01\xff<>", 4)) && !mm::search(""), "odd search words are no search");
  }
  // Stage files named for their post (the real site's "GrNLaWaffle.dat", Waffle House FD): the slot
  // their name starts with, else the card's stage; never a guess, never for a name the disc has.
  namespace mb = host::mod_browser;
  check(mb::stage_slot_for("GrNLaWaffle.dat", "") == "GrNLa.dat", "stage by name start");
  check(mb::stage_slot_for("GrPs1Lava.dat", "") == "GrPs1.dat", "stage by the longest name start");
  check(mb::stage_slot_for("WaffleHouse.dat", "Final Destination") == "GrNLa.dat", "stage by the card's stage");
  check(mb::stage_slot_for("WaffleHouse.dat", "Pokemon Stadium").empty(), "a stage name of several slots");
  check(mb::stage_slot_for("GrNLa.dat", "Final Destination").empty(), "a disc name failed for another reason");
  check(mb::stage_slot_for("PlKpDb.dat", "").empty(), "a costume file");
  check(mb::stage_slot_for("Gr.dat", "").empty() && mb::stage_slot_for("", "").empty(), "short names");
  std::printf("mods menu fuzz: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
