// Corpus run of costume_served_form (host/costume_served_form.h): every skin of a list against the
// retail file of its own slot, read from the retail disc. Not a ctest test; tools/skin_corpus.py
// extracts the skins and runs this.
//
//   port_skin_corpus <retail disc> <list.tsv> <out.csv>
//
// list.tsv: one skin per line, "<label>\t<path of the costume DAT>[\t<slot file>]"; the optional slot
// plays the skin in another color's retail slot (PlCaGr.dat), as the client does when a player wears
// it in another color. out.csv: one row per skin. The verdict tables go to stdout: the importer's
// tolerance (the default policy, offline's first try), the rule an opponent's skin gets and a player's
// own is announced by (host::skins::peer_skin_policy), and the rules a player's own skin is played by
// on their own screen online: as a costume slot (wearer_form, own_skin_policy) and in a disc slot
// (cosmetic_mods.h costume_online_form; for a skin in the slot it was made for that the importer takes).
// For the peer rule, the rest poses past the importer's tolerance are counted: the same placement
// written as other angles, or placed elsewhere (the skin's mesh then drawn on the retail bone).
// Exit 0 when every skin got a verdict, every served form's skeleton is the retail one bit for bit, and
// both own rules agree with the peer rule on every skin (online: shown on both machines, or on neither).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "costume_served_form.h"
#include "cosmetic_mods.h"
#include "dat_check.h"
#include "disc_image_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace host {
void log(const char*, ...) {}
bool disc_find_file(const std::string&, uint32_t*, uint32_t*) { return false; }
bool disc_read(uint32_t, void*, uint32_t) { return false; }
}

namespace {
std::string verdict_of(const host::skins::ServedForm& r) {
  if (r.ok) return "ok";
  const std::string& w = r.reason;
  if (w.rfind("no retail slot", 0) == 0) return "no retail slot";
  if (w.find("flags differ") != std::string::npos) return "flags differ";
  if (w.find("rest pose differs") != std::string::npos) return "rest pose beyond tolerance";
  if (w.find("joint count differs") != std::string::npos || w.find("hierarchy differs") != std::string::npos) return "skeleton shape differs";
  if (w.find("unknown") != std::string::npos || w.find("missing root") != std::string::npos) return "roots differ from retail";
  if (w.rfind("structure", 0) == 0) return "not a whole archive";
  if (w.find("not a ") != std::string::npos) return "not a costume";
  if (w.find("draw safety") != std::string::npos) return "draw-unsafe";
  if (w.find("constraint") != std::string::npos) return "joint constraint";
  if (w.find("class name differs") != std::string::npos) return "joint class name";
  if (w.find("display objects") != std::string::npos) return "too many display objects";
  return "other: " + w;
}
std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}
std::string csv(std::string s) {
  for (char& c : s) if (c == '"') c = '\'';
  return "\"" + s + "\"";
}
bool read_file(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) { std::fprintf(stderr, "usage: port_skin_corpus <retail disc> <list.tsv> <out.csv>\n"); return 2; }
  host::DiscImageReader disc(argv[1]);
  std::ifstream list(argv[2]);
  std::ofstream out(argv[3]);
  out << "label,source_slot,verdict,joints,skeleton_roots,snapped,peer_verdict,peer_past_tolerance,peer_placed_elsewhere,"
         "own_verdict,disc_verdict,ms,reason\n";
  std::map<std::string, std::vector<uint8_t>> retail_cache;
  std::map<std::string, int> table, peer_table, own_table;
  std::vector<double> times;
  int rows = 0, not_identical = 0, unreadable = 0, peer_rewritten = 0, peer_elsewhere = 0;
  int own_accepted = 0, peer_accepted = 0, own_disagree = 0, disc_compared = 0, disc_accepted = 0, disc_disagree = 0;
  std::string line;
  while (std::getline(list, line)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const size_t tab2 = line.find('\t', tab + 1);
    const std::string label = line.substr(0, tab), path = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
    const std::string played = tab2 == std::string::npos ? std::string() : line.substr(tab2 + 1);
    std::vector<uint8_t> skin;
    ++rows;
    if (!read_file(path, &skin)) { ++unreadable; ++table["unreadable"]; continue; }
    // The skin's own slot, when it has a retail one: its target is that slot (as the client's online
    // check of an installed skin). A skin made for an extra color names none.
    const auto identity = host::cosmetics::testing::inspect_dat(skin);
    std::string slot = !played.empty() ? played : identity.ok ? identity.target_path : std::string();
    host::skins::ServedForm r, peer, own;
    std::string disc_verdict = "n/a";
    double ms = 0;
    if (slot.empty()) {
      // Still run the rule, against the default colour's file of the fighter the roots name, for its
      // reason; the verdict of an unidentified skin is "no retail slot" or "not a costume" either way.
      const std::string structure = host::dat::structure_error(skin.data(), skin.size(), host::dat::Rules::Cosmetic);
      bool fighter_costume = false;
      for (const auto& root : identity.roots) fighter_costume = fighter_costume || (root.rfind("Ply", 0) == 0 && root.find("5K") != std::string::npos && root.size() > 12 && root.compare(root.size() - 12, 12, "_Share_joint") == 0);
      r.reason = !structure.empty() ? "structure: " + structure
               : fighter_costume ? "no retail slot: " + identity.error : "not a costume: no fighter skeleton root";
      peer = own = r;
    } else {
      auto cached = retail_cache.find(slot);
      if (cached == retail_cache.end()) {
        uint32_t offset = 0, size = 0;
        std::vector<uint8_t> bytes;
        if (!disc.find_file(slot, &offset, &size) || !disc.read_file(slot, size, bytes)) bytes.clear();
        cached = retail_cache.emplace(slot, std::move(bytes)).first;
      }
      const auto start = std::chrono::steady_clock::now();
      r = host::skins::costume_served_form(cached->second, skin, slot);
      ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      times.push_back(ms);
      peer = host::skins::costume_served_form(cached->second, skin, slot, host::skins::peer_skin_policy());
      own = host::skins::wearer_form(cached->second, skin, slot, true);
      if (peer.ok && peer.past_tolerance) (peer.placed_elsewhere ? peer_elsewhere : peer_rewritten) += 1;
      for (const auto* form : {&r, &peer, &own})
        if (form->ok && !host::skins::skeleton_identical(cached->second, form->bytes, nullptr)) {
          ++not_identical;
          std::fprintf(stderr, "NOT IDENTICAL: %s\n", label.c_str());
        }
      own_accepted += own.ok; peer_accepted += peer.ok;
      if (own.ok != peer.ok || own.bytes != peer.bytes) {
        ++own_disagree;
        std::fprintf(stderr, "OWN AND PEER DIFFER: %s (own: %s; peer: %s)\n", label.c_str(), own.reason.c_str(), peer.reason.c_str());
      }
      // In a disc slot: the skin in the slot it was made for, when the importer takes it there.
      std::string unsafe;
      if (identity.ok && lower(identity.target_path) == lower(slot) &&
          host::cosmetics::testing::costume_structure_safe(skin, &cached->second, &unsafe)) {
        std::vector<uint8_t> fixed;
        std::string detail;
        const bool disc_ok = host::cosmetics::costume_online_form(cached->second, skin, &fixed, &detail);
        ++disc_compared; disc_accepted += disc_ok;
        disc_verdict = disc_ok ? "ok" : host::cosmetics::online_reason_short(detail);
        if (disc_ok && !host::skins::skeleton_identical(cached->second, fixed, nullptr)) {
          ++not_identical;
          std::fprintf(stderr, "NOT IDENTICAL (disc slot): %s\n", label.c_str());
        }
        if (disc_ok != peer.ok || (disc_ok && fixed != peer.bytes)) {
          ++disc_disagree;
          std::fprintf(stderr, "DISC SLOT AND PEER DIFFER: %s (disc: %s; peer: %s)\n", label.c_str(), detail.c_str(), peer.reason.c_str());
        }
      }
    }
    const std::string verdict = verdict_of(r), peer_verdict = verdict_of(peer), own_verdict = verdict_of(own);
    ++table[verdict]; ++peer_table[peer_verdict]; ++own_table[own_verdict];
    char ms_text[32]; std::snprintf(ms_text, sizeof ms_text, "%.3f", ms);
    out << csv(label) << "," << slot << "," << csv(verdict) << "," << r.joints << "," << r.skeleton_roots << "," << r.snapped
        << "," << csv(peer_verdict) << "," << peer.past_tolerance << "," << peer.placed_elsewhere << "," << csv(own_verdict)
        << "," << csv(disc_verdict) << "," << ms_text << "," << csv(peer.reason) << "\n";
  }
  std::sort(times.begin(), times.end());
  std::printf("skins: %d (unreadable %d)\n", rows, unreadable);
  std::printf("\nverdict (default policy: the client's tolerance)        count\n");
  for (const auto& row : table) std::printf("  %-50s %5d\n", row.first.c_str(), row.second);
  std::printf("\nverdict (an opponent's skin: peer_skin_policy)           count\n");
  for (const auto& row : peer_table) std::printf("  %-50s %5d\n", row.first.c_str(), row.second);
  std::printf("  of those served with a rest pose past the tolerance: %d the same placement as other angles, %d placed elsewhere\n",
              peer_rewritten, peer_elsewhere);
  std::printf("\nverdict (a player's own skin online: wearer_form, own rule)  count\n");
  for (const auto& row : own_table) std::printf("  %-50s %5d\n", row.first.c_str(), row.second);
  std::printf("\nonline, own vs peer: own accepts %d, peer accepts %d, %d disagree (verdict or bytes)\n", own_accepted,
              peer_accepted, own_disagree);
  std::printf("online, disc slot vs peer: %d compared (own slot, importer takes it), %d accepted, %d disagree\n",
              disc_compared, disc_accepted, disc_disagree);
  if (!times.empty())
    std::printf("\nserved form time: median %.3f ms, max %.3f ms over %zu skins with a retail slot\n", times[times.size() / 2],
                times.back(), times.size());
  std::printf("served forms whose skeleton is not retail's bit for bit: %d\n", not_identical);
  for (const auto& retail : retail_cache)
    if (retail.second.empty()) std::printf("retail file missing on the disc: %s\n", retail.first.c_str());
  return not_identical || unreadable || own_disagree || disc_disagree ? 1 : 0;
}
