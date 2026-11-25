// The Mods menu's cards: the site's posts and files read into cards and packs, and their labels.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {

int number_of(const json& j, const char* key) {
  if (!j.is_object()) return 0;
  const auto it = j.find(key);
  if (it == j.end()) return 0;
  if (it->is_number_integer()) return (int)std::clamp<int64_t>(it->get<int64_t>(), 0, INT32_MAX);
  if (it->is_string()) {
    const std::string s = it->get<std::string>();
    if (s.empty() || s.size() > 9 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    return std::stoi(s);
  }
  return 0;
}
// A text field of the site's answer: "" when missing, null or not text; at most `max` bytes (cut
// before a character's continuation bytes).
std::string text_of(const json& j, const char* key, size_t max) {
  if (!j.is_object()) return {};
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  const std::string& s = it->get_ref<const std::string&>();
  if (s.size() <= max) return s;
  size_t n = max;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

// The host of an address, for the log.
std::string log_host(const std::string& url) {
  const size_t start = url.find("://");
  if (start == std::string::npos) return "?";
  const size_t end = url.find('/', start + 3);
  return url.substr(start + 3, end == std::string::npos ? std::string::npos : end - start - 3);
}

std::string encode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

// The file's size and hash as the site gives them: both or neither (64 hex digits, a size >= 0).
void hash_of(const json& j, Card& c) {
  std::string sha = text_of(j, "sha256");
  for (auto& ch : sha) ch = (char)std::tolower((unsigned char)ch);
  const auto size = j.find("size");
  const bool hex = sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
  if (hex && size != j.end() && size->is_number_integer() && size->get<int64_t>() >= 0) {
    c.sha256 = sha;
    c.size = size->get<int64_t>();
  }
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// A picture address worth trying: https (or the test stand-in), and a kind the reader takes (WebP
// and GIF are not).
bool picture_address(const std::string& url) {
  if (url.empty() || url.size() > 1024 || (url.rfind("https://", 0) != 0 && url.rfind("http://127.0.0.1:", 0) != 0)) return false;
  std::string lowered = url.substr(0, url.find('?'));
  for (auto& c : lowered) c = (char)std::tolower((unsigned char)c);
  const auto ends = [&](const char* e) {
    const size_t n = std::strlen(e);
    return lowered.size() >= n && lowered.compare(lowered.size() - n, n, e) == 0;
  };
  return !ends(".webp") && !ends(".gif");
}
void add_picture(Card& card, const std::string& url) {
  if (card.pictures.size() >= 6 || !picture_address(url)) return;
  if (std::find(card.pictures.begin(), card.pictures.end(), url) == card.pictures.end()) card.pictures.push_back(url);
}
bool flag_of(const json& j, const char* key) {
  const auto it = j.is_object() ? j.find(key) : j.end();
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

// The cards of one post of /mods: its character_dat (or stage_dat) files, narrowed to one character.
// A file with no download of its own (a mod uploaded through the site) is a card too: it is
// installed through the site's per-file export (install_one). Every field may be missing, null, of
// another type or too long: text_of and number_of read what is there or nothing.
void cards_of_post(const json& post, int category, const std::string& character, std::vector<Card>& out) {
  if (!post.is_object()) return;
  const auto files = post.find("files");
  if (files == post.end() || !files->is_array()) return;
  std::vector<Card> found;
  for (const auto& f : *files) {
    if (!f.is_object() || found.size() >= kMaxCardsPerPost) continue;
    const std::string type = text_of(f, "file_type");
    // Menus: a file is a card by its name (a menu screen this game takes: menu_card_file), whatever
    // its file_type.
    // New Stages: a stage file made over a stage new stages are played over, with a download of its own.
    if (category == kNewStages) {
      if (!new_stage_card_file(text_of(f, "filename", 512), text_of(post, "stage", 64)) ||
          text_of(f, "download_url", 1024).empty())
        continue;
    } else if (category == kMenus ? !menu_card_file(text_of(f, "filename", 512)) : type != file_type(category)) {
      continue;
    }
    Card c;
    c.file_id = number_of(f, "id");
    c.mod_id = number_of(post, "id");
    if (!c.file_id || !c.mod_id) continue;
    c.kind = asset_kind(category);
    c.character = text_of(f, "character", 64);
    if (!character.empty() && fighter_key(c.character) != fighter_key(character)) continue;
    c.downloads = number_of(post, "download_count");
    c.title = text_of(post, "title", 200);
    c.description = text_of(post, "description", 500);
    c.author = text_of(post, "author", 100);
    c.creator = text_of(post, "creator", 100);
    c.color = text_of(f, "color", 64);
    c.file_name = text_of(f, "filename", 512);
    c.stage = category == kStages ? stage_of_file(c.file_name, text_of(post, "stage", 64)) : std::string();
    if (category == kNewStages) c.stage_file = host::new_stages::base_of(c.file_name, text_of(post, "stage", 64));
    c.download_url = text_of(f, "download_url", 1024);
    c.export_url = text_of(f, "export_url", 1024);
    hash_of(f, c);
    // The viewer's picture, best first: the file's own portrait, its preview, its stock icon, the
    // post's thumbnail; a stage or effects card (no portrait) also the post's screenshots. Else a plain tile.
    const std::string csp = text_of(f, "csp_url", 1024), stock = text_of(f, "stock_url", 1024);
    if (picture_address(csp)) c.csp_url = csp;
    if (picture_address(stock)) c.stock_url = stock;
    add_picture(c, csp);
    add_picture(c, text_of(f, "preview_url", 1024));
    add_picture(c, stock);
    add_picture(c, text_of(post, "thumbnail_url", 1024));
    if (category != kCharacters)
      if (const auto shots = post.find("screenshots"); shots != post.end() && shots->is_array())
        for (const auto& s : *shots)
          if (s.is_string()) add_picture(c, s.get<std::string>());
    // The site's word that this file's picture is another file's (planned: shares_portrait or
    // duplicate_portrait); see also below.
    c.shares_portrait = flag_of(f, "shares_portrait") || flag_of(f, "duplicate_portrait");
    if (c.download_url.empty() && c.file_name.empty()) continue;   // neither a download nor a file the export can name
    found.push_back(std::move(c));
  }
  // Files of one post whose portrait is the same picture: it shows one of them at most, so none of
  // them is said to look like it.
  for (auto& c : found)
    if (!c.csp_url.empty())
      for (const auto& other : found)
        if (&other != &c && other.csp_url == c.csp_url) c.shares_portrait = true;
  for (auto& c : found) {
    c.multiple = found.size() > 1;
    out.push_back(std::move(c));
  }
}

// A path's parts ("Pack/Fox/PlFxNr.dat" -> Pack, Fox, PlFxNr), extensions left off.
std::vector<std::string> parts_of(const std::string& path) {
  std::vector<std::string> parts;
  std::string part;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/' || path[i] == '\\') {
      const size_t dot = part.find('.');
      if (dot != std::string::npos && dot > 0) part = part.substr(0, dot);
      if (!part.empty() && (parts.empty() || lower(parts.back()) != lower(part))) parts.push_back(part);
      part.clear();
    } else {
      part += path[i];
    }
  }
  return parts;
}

// A path's words: its parts (parts_of), each cut at - _ and spaces ("Goku/PlCaRe-GOKU-FALCON-HALO.dat"
// -> Goku, PlCaRe, GOKU, FALCON, HALO).
std::vector<std::string> words_of(const std::string& path) {
  std::vector<std::string> words;
  for (const auto& part : parts_of(path)) {
    std::string word;
    for (size_t i = 0; i <= part.size(); ++i) {
      if (i == part.size() || part[i] == '-' || part[i] == '_' || part[i] == ' ') {
        if (!word.empty()) words.push_back(word);
        word.clear();
      } else {
        word += part[i];
      }
    }
  }
  return words;
}

// A file's own name without its folders and extension ("PlCaRe-GOKU-FALCON-HALO").
std::string stem_of(const Card& c) {
  const auto parts = parts_of(c.file_name);
  return parts.empty() ? std::string() : parts.back();
}

// A pack's rows: labelled by fighter and colour ("C. Falcon - Red"; a stage card by its stage), never
// by the pack's own title. The colour is the one the file's own name says (the slot the game puts it
// in), else the site's; when only the site's is known and may be another file's (portrait_shared), the
// file's own name ("PlMrSb"). Cards that would read the same get what tells their files apart, with
// what all their names share at the start and end left out ("C. Falcon - Red" and "C. Falcon - Red -
// HALO"); never two the same. In the game's roster order, then colour.
void label_pack(std::vector<Card>& cards, int category) {
  for (auto& c : cards) {
    std::string label;
    if (category == kStages) {
      label = c.stage;
    } else if (category == kEffects) {
      // An effects pack's rows: whose effects each is ("Fox", "C. Falcon"), else the file's own name.
      label = fighter_label(c.character);
    } else {
      const std::string who = climber_of(c) ? "Ice Climbers" : fighter_label(c.character);
      const std::string colour = colour_text(c);
      if (!colour.empty()) label = who.empty() ? colour : who + " - " + colour;
      else if (!c.shares_portrait) label = who;
    }
    if (label.empty()) {
      const std::string stem = stem_of(c);
      label = stem.empty() ? "Skin" : stem;
    }
    c.label = label;
    c.variant.clear();
    c.same_colour = false;
  }
  std::map<std::string, std::vector<Card*>> same;
  for (auto& c : cards) same[lower(c.label)].push_back(&c);
  for (auto& entry : same) {
    auto& group = entry.second;
    if (group.size() < 2) continue;
    std::vector<std::vector<std::string>> words;
    size_t shortest = SIZE_MAX;
    for (auto* c : group) {
      c->same_colour = true;
      words.push_back(words_of(c->file_name));
      shortest = std::min(shortest, words.back().size());
    }
    const auto same_at = [&](size_t k, bool from_end) {
      for (const auto& w : words) {
        const auto& a = from_end ? w[w.size() - 1 - k] : w[k];
        const auto& b = from_end ? words[0][words[0].size() - 1 - k] : words[0][k];
        if (lower(a) != lower(b)) return false;
      }
      return true;
    };
    size_t lead = 0, trail = 0;
    while (lead < shortest && same_at(lead, false)) ++lead;
    while (trail + lead < shortest && same_at(trail, true)) ++trail;
    std::vector<std::string> variants(group.size());
    int plain = 0;
    for (size_t i = 0; i < group.size(); ++i) {
      for (size_t k = lead; k + trail < words[i].size(); ++k) variants[i] += (variants[i].empty() ? "" : " ") + words[i][k];
      plain += variants[i].empty() ? 1 : 0;
    }
    // One whose name is all the others' share keeps the plain label; several such are numbered.
    int number = 0;
    for (size_t i = 0; i < group.size(); ++i) {
      if (variants[i].empty() && plain > 1) variants[i] = std::to_string(++number);
      if (variants[i].empty()) continue;
      group[i]->variant = variants[i];
      group[i]->label += " - " + variants[i];
    }
  }
  // Never two rows the same (two files of the same name in folders the words above do not tell apart).
  std::map<std::string, int> seen;
  for (auto& c : cards) {
    const int n = ++seen[lower(c.label)];
    if (n > 1) c.label += " - " + std::to_string(n);
  }
  std::stable_sort(cards.begin(), cards.end(), [&](const Card& a, const Card& b) {
    if (by_fighter(category)) {
      const int ra = roster_index(a.character), rb = roster_index(b.character);
      if (ra != rb) return ra < rb;
    }
    if (category == kCharacters) {
      const int ca = colour_index(a), cb = colour_index(b);
      if (ca != cb) return ca < cb;
    }
    return lower(a.label) < lower(b.label);
  });
}

// A post of /mods (or /mods/{id}'s data): its description and the screenshots this reader may take (a
// pack's own page shows the first).
void post_words_of(const json& item, Post& post) {
  post.description = text_of(item, "description", 500);
  post.shots.clear();
  if (const auto shots = item.find("screenshots"); shots != item.end() && shots->is_array())
    for (const auto& s : *shots)
      if (s.is_string() && post.shots.size() < 3 && picture_address(s.get_ref<const std::string&>()))
        post.shots.push_back(s.get<std::string>());
  post.shots_known = true;
}

// One post of /mods as a row of Browse: its cards in this list (none: no row), as read (finish_post
// pairs, labels and judges them).
Post post_of(const json& item, int category, const std::string& character) {
  Post post;
  if (!item.is_object()) return post;
  post.mod_id = number_of(item, "id");
  post.downloads = number_of(item, "download_count");
  post.title = text_of(item, "title", 200);
  post.author = text_of(item, "author", 100);
  post.creator = text_of(item, "creator", 100);
  cards_of_post(item, category, character, post.cards);
  post_words_of(item, post);
  return post;
}

// Two cards' files are not the same file: their hashes differ when the site has both, else their names.
bool other_file(const Card& a, const Card& b) {
  if (a.file_id == b.file_id) return false;
  if (!a.sha256.empty() && !b.sha256.empty()) return a.sha256 != b.sha256;
  return lower(a.file_name) != lower(b.file_name);
}

// Which of a post's pictures are another file's (the Sunshine Mario pack: five files, one csp_url):
// those are not kept with a skin, and the viewer says so. A card's own address is its own picture even
// when the site marks it portrait_shared (DBZ Pack Goku: twelve files, twelve portraits, each its own
// file's, all marked). Ice Climbers' Nana is Popo's partner here, not a sibling: one costume, one
// portrait. Only the cards read are compared (a pack's own list holds them all).
void judge_pictures(std::vector<Card>& cards) {
  for (auto& c : cards) {
    c.withhold_csp = c.withhold_stock = false;
    for (const auto& other : cards) {
      if (&other == &c || !other_file(c, other)) continue;
      if (!c.csp_url.empty() && other.csp_url == c.csp_url) c.withhold_csp = true;
      if (!c.stock_url.empty() && other.stock_url == c.stock_url) c.withhold_stock = true;
    }
  }
}

// The viewer's pictures of a card, best first: its own portrait, its preview, its stock icon, the
// post's thumbnail (then the plain tile).
void card_pictures(const json& j, Card& c) {
  const std::string csp = text_of(j, "csp_url", 1024), stock = text_of(j, "stock_url", 1024);
  if (picture_address(csp)) c.csp_url = csp;
  if (picture_address(stock)) c.stock_url = stock;
  add_picture(c, csp);
  add_picture(c, text_of(j, "preview_url", 1024));
  add_picture(c, stock);
  add_picture(c, text_of(j, "thumbnail_url", 1024));
}

// One card of /cards (docs/public-api.md, its card fields): false when it is not one of this list
// (another type, no ids, neither a download nor a file the export can name). Like cards_of_post, every
// field may be missing, null, of another type or too long.
bool card_of(const json& j, int category, Card& c) {
  if (!j.is_object() || text_of(j, "type", 32) != site_type(category)) return false;
  c.file_id = number_of(j, "id");
  c.mod_id = number_of(j, "mod_id");
  if (!c.file_id || !c.mod_id) return false;
  c.kind = asset_kind(category);
  c.character = text_of(j, "character", 64);
  c.downloads = number_of(j, "download_count");
  c.title = text_of(j, "title", 200);
  c.description = text_of(j, "description", 500);
  c.author = text_of(j, "author", 100);
  c.creator = text_of(j, "creator", 100);
  c.color = text_of(j, "color", 64);
  c.file_name = text_of(j, "filename", 512);
  c.download_url = text_of(j, "download_url", 1024);
  c.export_url = text_of(j, "export_url", 1024);
  hash_of(j, c);
  // A card the site hides as a duplicate portrait has no download_url (its export installs it); its
  // picture, stock icon and colour may be another file's, as may those of every card marked
  // portrait_shared.
  c.hidden_duplicate = flag_of(j, "hidden_duplicate");
  if (c.hidden_duplicate) c.download_url.clear();
  c.shares_portrait = flag_of(j, "portrait_shared");
  if (category == 1) {
    // The stage it replaces: the site's stage_file (worked out as its export does), else guessed from
    // the file's name as before.
    std::string name;
    c.stage_file = stage_slot_of(text_of(j, "stage_file", 64), &name);
    c.stage = !c.stage_file.empty() ? name : stage_of_file(c.file_name, text_of(j, "stage", 64));
  }
  const int with_duplicates = kListHiddenDuplicates ? number_of(j, "mod_card_count_with_duplicates") : 0;
  c.mod_cards = with_duplicates > 0 ? with_duplicates : number_of(j, "mod_card_count");
  card_pictures(j, c);
  // An effects card has no portrait: its preview, then its mod's screenshots (as /cards gives them).
  if (category == kEffects)
    if (const auto shots = j.find("screenshots"); shots != j.end() && shots->is_array())
      for (const auto& s : *shots)
        if (s.is_string()) add_picture(c, s.get<std::string>());
  // (a hidden card the site sends all the same is no row while they stay hidden)
  if (c.hidden_duplicate && !kListHiddenDuplicates) return false;
  return !(c.download_url.empty() && c.file_name.empty());
}

// The cards of one post as its row and its pack's list show them: Nana's file with Popo's, labelled
// and sorted (a pack), its pictures judged, counted.
void finish_post(Post& post, int category) {
  pair_climbers(post.cards);
  if (post.cards.size() > 1) label_pack(post.cards, category);
  for (auto& c : post.cards) c.multiple = post.cards.size() > 1;
  judge_pictures(post.cards);
  post.count = (int)post.cards.size() + post.hidden_more;
}

// A reply's items into a list's posts. /cards: a post's cards come one after another (the site keeps
// them together), and a post whose cards
// go on in the next reply is the same post. /mods: a post each, its files narrowed to the category and
// the fighter (`character`; a post with none is no row). False when the reply is not a list.
bool stream_add(const json& j, int category, const std::string& character, Stream& s) {
  if (!j.is_object()) return false;
  const auto data = j.find("data");
  if (data == j.end() || !data->is_array()) return false;
  for (const auto& item : *data) {
    if (s.posts.size() >= kMaxStreamPosts) break;
    if (!s.from_cards) {
      Post post = post_of(item, category, character);
      if (post.mod_id <= 0 || post.cards.empty()) continue;
      // (a post the list has already, should the site's order shift between batches: one row)
      if (std::any_of(s.posts.begin(), s.posts.end(), [&](const Post& p) { return p.mod_id == post.mod_id; })) continue;
      s.posts.push_back(std::move(post));
      continue;
    }
    Card c;
    if (!card_of(item, category, c)) continue;
    Post* post = !s.posts.empty() && s.posts.back().mod_id == c.mod_id ? &s.posts.back() : nullptr;
    if (!post) {
      // (a post seen earlier, should the site ever part its cards: they join it)
      for (auto& p : s.posts)
        if (p.mod_id == c.mod_id) post = &p;
    }
    if (!post) {
      Post fresh;
      fresh.mod_id = c.mod_id;
      fresh.downloads = c.downloads;
      fresh.title = c.title;
      fresh.author = c.author;
      fresh.creator = c.creator;
      fresh.description = c.description;   // (its screenshots: /mods/{id}, when its page is opened)
      s.posts.push_back(std::move(fresh));
      post = &s.posts.back();
    }
    if (post->cards.size() < kMaxCardsPerPost) post->cards.push_back(std::move(c));
  }
  s.next_cursor = text_of(j, "next_cursor", 512);
  s.done = s.next_cursor.empty() || s.posts.size() >= kMaxStreamPosts;
  s.total = std::max(number_of(j, "total"), 0);
  ++s.requests;
  return true;
}

// A list as one continuous list: `page` + 1 batches of kScreenPosts posts read so far, one row each
// (the list grows a batch at a time as the player scrolls down to its end). `before`, when given, is
// the same list one batch shorter, already finished: its posts are kept but for its last, which is
// finished again (a /cards post may have gone on into this batch). A /mods post's count is its files
// of the category (its own list, once read, counts them; post_count_locked). A /cards post holds all
// its cards of this list, except the list's last post while more is to come: there the site's count
// of its cards stands (mod_card_count, every type) until its own list is read.
Page screen_of(const Stream& s, int category, int page, int* cards, int* packs, const Page* before) {
  Page out;
  out.status = kReady;
  out.from_cards = s.from_cards;
  out.per_page = kScreenPosts;
  out.total = s.total;
  const size_t last = std::min(s.posts.size(), (size_t)(page + 1) * kScreenPosts);
  size_t i = 0;
  if (before != nullptr && before->status == kReady && !before->posts.empty()) {
    const size_t kept = std::min(before->posts.size() - 1, last);
    out.posts.assign(before->posts.begin(), before->posts.begin() + (std::ptrdiff_t)kept);
    i = kept;
  }
  for (; i < last; ++i) {
    Post post = s.posts[i];
    const size_t listed = post.cards.size();
    const int mod_cards = post.cards.empty() ? 0 : post.cards.front().mod_cards;
    finish_post(post, category);
    const bool edge = s.from_cards && !s.done && i + 1 == s.posts.size();
    if (edge && mod_cards > (int)listed) post.count = mod_cards;
    out.posts.push_back(std::move(post));
  }
  for (const auto& post : out.posts) {
    *cards += (int)post.cards.size();
    *packs += post.count > 1 ? 1 : 0;
  }
  out.has_next = s.posts.size() > last || !s.done;
  if (out.posts.empty()) out.message = "Nothing here yet.";
  return out;
}

}  // namespace menu_internal
}  // namespace host::mods_menu
