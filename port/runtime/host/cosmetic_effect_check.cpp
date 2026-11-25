// An effect mod checked (cosmetic_mods.h, EffectVerdict): what its files do in this game, decided from
// their contents against the disc's own files, never by playing a match. One file's verdict
// (judge_effect_dat) is what import_effect takes or refuses a file by, so the Mods menu and
// --check-effect / --check-effects (cosmetics_cli.cpp) cannot disagree.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"
#include "effect_slots.h"

namespace host::cosmetics {
namespace {

void set(EffectVerdict* v, const char* verdict, const std::string& reason, const std::string& detail) {
  v->verdict = verdict; v->reason = reason; v->detail = detail;
}

// What a game file that holds no effect is, from its roots ("a character skin (Fox, Green)"); empty
// when it is none this knows.
std::string what_it_is(const std::vector<uint8_t>& bytes, const std::vector<std::string>& roots) {
  const testing::DatInspection costume = inspect_dat_impl(bytes);
  if (costume.ok) return "a character skin (" + costume.character + ", " + costume.costume + ")";
  const auto any = [&](const auto& match) { return std::any_of(roots.begin(), roots.end(), match); };
  if (any([](const std::string& r) { return r.rfind("Ply", 0) == 0; })) return "a character skin";
  if (any([](const std::string& r) { return r == "map_head" || r == "coll_data" || r.rfind("grGround", 0) == 0; }))
    return "a stage";
  if (any([](const std::string& r) { return r.rfind("Mn", 0) == 0; })) return "a menu screen";
  return {};
}

// A fighter's own file (PlPe.dat) the effect checks refuse: what it changes instead, from the fields of
// its fighter data (ft/types.h struct ftData) that differ from the disc's. x48 is the fighter's items
// (articles: Peach's turnips, Link's bombs), so a file that changes only those swaps their models.
std::string fighter_file_change(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& bytes,
                                const std::string& root, const std::string& who, std::string* detail) {
  std::vector<uint32_t> fields;
  std::string error;
  if (!effect_slots::changed_fields(clean, bytes, root, 0x60, &fields, &error) || fields.empty()) return {};
  *detail += " " + root + " fields that differ:";
  for (uint32_t f : fields) {
    char text[8];
    std::snprintf(text, sizeof text, " x%X", f);
    *detail += text;
  }
  if (fields == std::vector<uint32_t>{0x48}) return "a model swap of " + who + "'s items";
  return "a change to " + who + "'s fighter file";
}

int rank(const std::string& verdict) {
  return verdict.empty() ? 4 : verdict == "broken" ? 3 : verdict == "not_an_effect" ? 2 : verdict == "works_after_restart" ? 1 : 0;
}

}  // namespace

void judge_effect_dat(const std::string& filename, const std::vector<uint8_t>& bytes, EffectVerdict* v) {
  *v = EffectVerdict{};
  v->file = filename;
  v->size = bytes.size();
  v->sha256 = sha256(bytes);
  // A whole archive, as the game's loader takes one (dat_check.h).
  std::vector<std::string> roots;
  const std::string damage = host::dat::structure_error(bytes.data(), bytes.size(), host::dat::Rules::Loader, &roots);
  if (!damage.empty()) {
    if (bytes.size() >= 0x20 && host::dat::animation_file_error(bytes.data(), bytes.size()).empty()) {
      v->really = "a fighter's animations";
      set(v, "not_an_effect", "Not an effect: it is " + v->really + ".", damage);
    } else {
      set(v, "broken", "It is damaged: not a whole game file.", damage);
    }
    return;
  }
  std::string target, who;
  if (!standalone_effect_target(roots, filename, &target, &who)) {
    const auto table = std::find_if(roots.begin(), roots.end(), [](const std::string& r) {
      return r.rfind("eff", 0) == 0 && r.size() > 9 && r.compare(r.size() - 9, 9, "DataTable") == 0;
    });
    std::string known;
    if (table != roots.end() && table->rfind("effKirby", 0) == 0 && *table != "effKirbyDataTable") {
      // Kirby's copy of another fighter's move ("effKirbyFoxDataTable", EfKbFx.dat): no effect target here.
      set(v, "broken", "Kirby's copied effects are not supported.", "effect table " + *table);
      return;
    }
    if (table != roots.end() && effect_target_from_roots({*table}, &known)) {
      // A fighter's table among other roots (a package's file under another name).
      set(v, "broken", "Not laid out as the game's file.",
          "effect table " + *table + " (" + known + ") among " + std::to_string(roots.size()) + " roots; the game's file has one");
      return;
    }
    if (table != roots.end()) {
      // An effect table of no fighter of this disc: an added fighter's ("effWolfDataTable").
      set(v, "broken", "Its fighter is not in this game.", "effect table " + *table + " is of no fighter of this disc");
      return;
    }
    v->really = what_it_is(bytes, roots);
    set(v, "not_an_effect", v->really.empty() ? "Not an effect file of this game." : "Not an effect: it is " + v->really + ".",
        "roots: " + (roots.empty() ? std::string("none") : roots[0]) + (roots.size() > 1 ? " and more" : ""));
    return;
  }
  v->target = target;
  v->character = who;
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(target, &offset, &size)) {
    // Every disc has the common effect file: without it no disc is open.
    if (!host::disc_find_file("EfCoData.dat", &offset, &size)) {
      set(v, "", "The game disc is needed to check it.", "no disc open");
    } else {
      set(v, "broken", "This game has no such effect file.", target + " is not on the disc");
    }
    return;
  }
  std::vector<uint8_t> runtime;
  std::string classification, error;
  if (!materialize_effect_with_open_disc(target, bytes, &runtime, &classification, &error)) {
    if (error.find("unavailable") != std::string::npos) { set(v, "", plain_effect_refusal(error), error); return; }
    // A fighter's own file but no effect of it (only Fox's and Falco's take particle colors): what it is.
    const std::string low = lower(target);
    if (fighter_base_target(low) && low != "plfx.dat" && low != "plfc.dat" && roots.size() == 1) {
      std::vector<uint8_t> clean(size);
      if (host::disc_read(offset, clean.data(), size)) v->really = fighter_file_change(clean, bytes, roots[0], who, &error);
      if (!v->really.empty()) { set(v, "not_an_effect", "Not an effect: it is " + v->really + ".", error); return; }
    }
    set(v, "broken", plain_effect_refusal(error), error);
    return;
  }
  set(v, effect_file_live(lower(target)) ? "works" : "works_after_restart", "", classification);
}

EffectVerdict check_effect(const std::string& path_text) {
  EffectVerdict mod;
  mod.file = path_text;
  const fs::path path = fs::u8path(path_text);
  std::error_code ec;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files;   // name, bytes
  std::string error;
  if (fs::is_directory(path, ec)) {
    // A folder is one mod: its DATs (not those of folders in it).
    for (const auto& entry : fs::directory_iterator(path, ec))
      if (entry.is_regular_file(ec) && dat_extension(path_filename_utf8(entry.path()))) {
        if (files.size() >= kMaxDatCandidates) { set(&mod, "broken", "Too many files in it.", "more than 32 DATs"); return mod; }
        std::vector<uint8_t> bytes;
        if (!read_bounded(entry.path(), kMaxAssetBytes, &bytes, &error)) { set(&mod, "", "It cannot be read.", error); return mod; }
        files.emplace_back(path_filename_utf8(entry.path()), std::move(bytes));
      }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  } else {
    const std::string extension = lower(path.extension().string());
    std::vector<uint8_t> whole;
    if (!read_bounded(path, kMaxAssetBytes, &whole, &error)) { set(&mod, "", "It cannot be read.", error); return mod; }
    mod.size = whole.size();
    mod.sha256 = sha256(whole);
    if (extension == ".dat" || extension == ".usd") {
      files.emplace_back(path_filename_utf8(path), std::move(whole));
    } else if (extension == ".zip") {
      Zip zip;
      if (!open_zip(path, &zip, &error)) { set(&mod, "broken", "It is damaged: not a whole zip.", error); return mod; }
      for (const auto& entry : zip.entries()) {
        if (!dat_extension(entry.name)) continue;
        if (files.size() >= kMaxDatCandidates) { set(&mod, "broken", "Too many files in it.", "more than 32 DATs"); return mod; }
        std::vector<uint8_t> bytes;
        if (!zip.read(entry, &bytes, &error)) { set(&mod, "broken", "It is damaged: a file in it cannot be read.", error); return mod; }
        files.emplace_back(entry.name, std::move(bytes));
      }
    } else {
      set(&mod, "not_an_effect", "Not an effect file of this game.", "not a .dat, .usd or .zip");
      return mod;
    }
  }
  if (files.empty()) { set(&mod, "not_an_effect", "No game file in it.", "no DAT"); return mod; }
  for (const auto& [name, bytes] : files) {
    EffectVerdict part;
    judge_effect_dat(path_filename_utf8(fs::u8path(name)), bytes, &part);
    if (files.size() > 1 || !fs::is_regular_file(path, ec) || lower(path.extension().string()) == ".zip")
      part.file = path_filename_utf8(path) + "/" + name;
    if (part.verdict == "works" || part.verdict == "works_after_restart") {
      std::lock_guard<std::mutex> lock(g_mutex);
      effect_would_take_locked(part.target, bytes, part.sha256, &part.slots, &part.whole, &part.overrides);
    }
    mod.parts.push_back(std::move(part));
  }
  // The mod: the worst of its files (an install takes all of them or none), the first of those.
  const EffectVerdict* worst = &mod.parts.front();
  for (const auto& part : mod.parts)
    if (rank(part.verdict) > rank(worst->verdict)) worst = &part;
  EffectVerdict out = *worst;
  out.file = path_text;
  if (!mod.sha256.empty()) { out.sha256 = mod.sha256; out.size = mod.size; }
  else if (mod.parts.size() > 1) { out.sha256.clear(); out.size = 0; }
  if (mod.parts.size() > 1) out.parts = std::move(mod.parts);
  return out;
}

std::string effect_verdict_json(const EffectVerdict& v) {
  using ordered = nlohmann::ordered_json;
  const auto one = [](const EffectVerdict& e, bool top) {
    ordered j;
    j["file"] = e.file;
    j["verdict"] = e.verdict.empty() ? "unchecked" : e.verdict;
    j["reason"] = e.reason;
    if (!e.really.empty()) j["really"] = e.really;
    j["character"] = e.character;
    j["target"] = e.target;
    j["slots"] = e.slots;
    j["whole"] = e.whole;
    ordered overrides = ordered::array();
    for (const auto& t : e.overrides) overrides.push_back({{"id", t.asset_id}, {"name", t.name}, {"slots", t.slots}});
    j["overrides"] = std::move(overrides);
    if (!e.sha256.empty()) { j["sha256"] = e.sha256; j["size"] = e.size; }
    j["detail"] = e.detail;
    if (top) j["checks"] = kEffectChecksVersion;
    return j;
  };
  ordered j = one(v, true);
  if (!v.parts.empty()) {
    ordered parts = ordered::array();
    for (const auto& p : v.parts) parts.push_back(one(p, false));
    j["files"] = std::move(parts);
  }
  return j.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
}

}  // namespace host::cosmetics
