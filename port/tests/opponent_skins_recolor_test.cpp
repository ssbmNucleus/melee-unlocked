// An opponent's skin in the color played (host/opponent_skins.h costume_of_slot, recolor_form): Teams
// puts a player in their team's color after the pick that named the skin, so the receiving machine is
// asked for another costume file of the same fighter than the one the skin was fetched for. It must
// serve the same skin there, built for that slot (its symbols that slot's, its skeleton and rest pose
// that slot's retail ones), not the retail costume. Synthetic costume files (as
// costume_served_form_test.cpp builds them); offline, no disc.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "opponent_skins.h"

#include "cosmetic_mods.h"
#include "costume_served_form.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); ++failures; }
  else std::printf("ok   %s\n", message.c_str());
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = (uint8_t)(x >> 24); v[at + 1] = (uint8_t)(x >> 16); v[at + 2] = (uint8_t)(x >> 8); v[at + 3] = (uint8_t)x;
}
uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// An HSD archive under construction: a data block, its relocations and its roots.
struct Dat {
  std::vector<uint8_t> data;
  std::vector<uint32_t> relocs;
  std::vector<std::pair<uint32_t, std::string>> roots;
  uint32_t alloc(uint32_t size) {
    const uint32_t at = (uint32_t)data.size();
    data.resize(data.size() + ((size + 3) & ~3u), 0);
    return at;
  }
  void word(uint32_t at, uint32_t value) { put32(data, at, value); }
  void ptr(uint32_t field, uint32_t target) { put32(data, field, target); relocs.push_back(field); }
  void root(uint32_t at, const std::string& name) { roots.emplace_back(at, name); }
  std::vector<uint8_t> build() const {
    std::vector<uint8_t> out(0x20, 0);
    out.insert(out.end(), data.begin(), data.end());
    for (uint32_t r : relocs) { out.resize(out.size() + 4); put32(out, out.size() - 4, r); }
    std::string strings;
    for (const auto& r : roots) {
      out.resize(out.size() + 8);
      put32(out, out.size() - 8, r.first);
      put32(out, out.size() - 4, (uint32_t)strings.size());
      strings += r.second; strings.push_back('\0');
    }
    out.insert(out.end(), strings.begin(), strings.end());
    put32(out, 0, (uint32_t)out.size());
    put32(out, 4, (uint32_t)data.size());
    put32(out, 8, (uint32_t)relocs.size());
    put32(out, 12, (uint32_t)roots.size());
    put32(out, 16, 0);
    return out;
  }
};

uint32_t joint(Dat& d, uint32_t flags, float rx, float ty) {
  const uint32_t j = d.alloc(0x40);
  d.word(j + 4, flags);
  d.word(j + 0x14, bits(rx));
  d.word(j + 0x20, bits(1)); d.word(j + 0x24, bits(1)); d.word(j + 0x28, bits(1));
  d.word(j + 0x30, bits(ty));
  const uint32_t m = d.alloc(0x30);
  d.ptr(j + 0x38, m);
  return j;
}

// A Fox costume for `colour` ("Nr", "Or", ...): root -> 1 -> {2, 3 beside it} -> 4 under 3, two display
// objects on joint 1, a matanim root, a few "texture" bytes; `rest_delta` moves joint 2's translation,
// `joint2_flags` are its flags.
std::vector<uint8_t> fox(const std::string& colour, float rest_delta, uint8_t texture, uint32_t joint2_flags = 0) {
  Dat d;
  const uint32_t t = d.alloc(0x20);
  for (uint32_t i = 0; i < 0x20; ++i) d.data[t + i] = (uint8_t)(texture + i);
  const uint32_t j0 = joint(d, 0x2, 0, 0), j1 = joint(d, 0, 0.25f, 1), j2 = joint(d, joint2_flags, 0, 2 + rest_delta);
  d.ptr(j0 + 0x08, j1);
  d.ptr(j1 + 0x08, j2);
  const uint32_t j3 = joint(d, 0, 0.5f, 3);
  d.ptr(j2 + 0x0C, j3);
  const uint32_t j4 = joint(d, 0, 0, 4);
  d.ptr(j3 + 0x08, j4);
  uint32_t previous = 0;
  for (int i = 0; i < 2; ++i) {
    const uint32_t dobj = d.alloc(0x10);
    if (i == 0) d.ptr(j1 + 0x10, dobj); else d.ptr(previous + 0x04, dobj);
    previous = dobj;
  }
  const std::string c = colour == "Nr" ? "" : colour;
  d.root(j0, "PlyFox5K" + c + "_Share_joint");
  const uint32_t matanim = d.alloc(0x20);
  d.root(matanim, "PlyFox5K" + c + "_Share_matanim_joint");
  return d.build();
}

bool has(const std::vector<uint8_t>& f, const std::string& s) { return std::search(f.begin(), f.end(), s.begin(), s.end()) != f.end(); }

uint32_t crc32_of(const std::vector<uint8_t>& b) {
  uint32_t c = 0xFFFFFFFFu;
  for (const uint8_t byte : b) {
    c ^= byte;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

// A zip of stored files (as skin_fetch_test.cpp builds one), names exactly as given (bytes, any code page).
std::vector<uint8_t> stored_zip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> out, central;
  const auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
  const auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xFFFF); u16(v, x >> 16); };
  for (const auto& [name, data] : files) {
    const uint32_t offset = (uint32_t)out.size(), crc = crc32_of(data), size = (uint32_t)data.size();
    u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0x21);
    u32(out, crc); u32(out, size); u32(out, size); u16(out, (uint32_t)name.size()); u16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
    u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0); u16(central, 0);
    u16(central, 0x21); u32(central, crc); u32(central, size); u32(central, size); u16(central, (uint32_t)name.size());
    u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
    central.insert(central.end(), name.begin(), name.end());
  }
  const uint32_t at = (uint32_t)out.size();
  out.insert(out.end(), central.begin(), central.end());
  u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, (uint32_t)files.size()); u16(out, (uint32_t)files.size());
  u32(out, (uint32_t)central.size()); u32(out, at); u16(out, 0);
  return out;
}

}  // namespace

int main() {
  using host::skins::costume_of_slot;
  bool partner = true;

  // Which costume a slot is, for a pick's fighter (CSS numbers: 2 Fox, 14 Ice Climbers, 18 Zelda, 19 Sheik).
  check(costume_of_slot(2, "PlFxOr.dat", false, &partner) == 1 && !partner, "Fox: PlFxOr.dat is costume 1");
  check(costume_of_slot(2, "plfxgr.dat", false, &partner) == 3 && !partner, "Fox: any case");
  check(costume_of_slot(2, "PlFcNr.dat", true, &partner) == -1, "Fox: Falco's file is none of his");
  check(costume_of_slot(2, "", true, &partner) == -1, "no slot");
  const std::string nana = host::skins::partner_slot(14, 1);
  check(!nana.empty() && costume_of_slot(14, nana, true, &partner) == 1 && partner, "Ice Climbers: Nana's " + nana + " is the partner of costume 1");
  check(costume_of_slot(14, nana, false, &partner) == -1, "Ice Climbers: no partner file kept, no partner slot");
  const std::string sheik = host::cosmetics::costume_slot_file(19, 2);
  check(!sheik.empty() && costume_of_slot(18, sheik, true, &partner) == 2 && partner, "Zelda: Sheik's " + sheik + " is the partner of costume 2");

  // The opponent picked Fox's default color with a skin made for it; Teams plays them in orange.
  host::skins::PortSkin remote;
  remote.state = host::skins::PortState::kReady;
  remote.character = 2;
  remote.costume = 0;
  remote.slot = "PlFxNr.dat";
  remote.generation = 7;
  const std::vector<uint8_t> skin = fox("Nr", 0, 0x77);
  remote.source = std::make_shared<const std::vector<uint8_t>>(skin);
  remote.bytes = remote.source;
  // Orange's retail skeleton differs a little from the default's (within the importer's tolerance).
  const std::vector<uint8_t> retail_or = fox("Or", 0.0005f, 0x11);
  // The lookup before 2026-11-24 asked for the picked file's name only: orange found nothing.
  check(remote.slot != "PlFxOr.dat", "the picked file is not the one played (the case that showed the retail costume)");
  std::string why;
  const auto served = host::skins::recolor_form(remote, "PlFxOr.dat", retail_or, &why);
  check(served != nullptr, "served in orange: " + why);
  if (served) {
    check(has(*served, "PlyFox5KOr_Share_joint") && has(*served, "PlyFox5KOr_Share_matanim_joint") && !has(*served, "PlyFox5K_Share"),
          "served in orange: its symbols are orange's");
    check(host::cosmetics::testing::inspect_dat(*served).target_path == "PlFxOr.dat", "served in orange: identifies as PlFxOr.dat");
    std::string differ;
    check(host::skins::skeleton_identical(retail_or, *served, &differ), "served in orange: the skeleton and rest pose are orange's retail ones " + differ);
    std::vector<uint8_t> marks;   // compared as bytes (a std::string's chars past 0x7F are negative)
    for (uint8_t i = 0; i < 0x20; ++i) marks.push_back((uint8_t)(0x77 + i));
    check(std::search(served->begin(), served->end(), marks.begin(), marks.end()) != served->end(),
          "served in orange: the skin's own textures");
  }
  // Not the fighter's costume, or no skin ready: nothing (the retail costume).
  // (Each call before its message: the reason printed is that call's own.)
  bool none = host::skins::recolor_form(remote, "PlFcOr.dat", retail_or, &why) == nullptr;
  check(none && why.find("no costume of this pick's fighter") != std::string::npos, "another fighter's slot: nothing (" + why + ")");
  host::skins::PortSkin working = remote;
  working.state = host::skins::PortState::kWorking;
  none = host::skins::recolor_form(working, "PlFxOr.dat", retail_or, &why) == nullptr;
  check(none, "a skin not ready: nothing (" + why + ")");
  host::skins::PortSkin no_source = remote;
  no_source.source.reset();
  none = host::skins::recolor_form(no_source, "PlFxOr.dat", retail_or, &why) == nullptr;
  check(none && why.find("no costume file kept") != std::string::npos, "no costume file kept: nothing (" + why + ")");
  // The color played's retail rest pose far from the skin's: served all the same, on that color's retail
  // skeleton (the peer rule, as in the color picked; before 2026-11-25 this was refused).
  {
    const std::vector<uint8_t> far_or = fox("Or", 0.5f, 0x11);
    const auto far = host::skins::recolor_form(remote, "PlFxOr.dat", far_or, &why);
    std::string differ;
    check(far && host::skins::skeleton_identical(far_or, *far, &differ),
          "beyond tolerance in the color played: served on that color's retail skeleton (" + why + differ + ")");
  }
  // What the simulation reads beyond the rest pose still refuses there: the color played's retail joint
  // has a non-visual flag (a quaternion rotation) the skin's does not.
  none = host::skins::recolor_form(remote, "PlFxOr.dat", fox("Or", 0, 0x11, 1u << 17), &why) == nullptr;
  check(none && why.find("served form") != std::string::npos && why.find("flags differ") != std::string::npos,
        "another non-visual flag in the color played: refused (" + why + ")");

  // A skin download whose files are named in a legacy code page (CP437 0x82, not UTF-8): the costume is
  // still found, and nothing throws (the fetch worker had no handler; the review of 2026-11-24).
  {
    const std::vector<uint8_t> orange = fox("Or", 0, 0x33);
    const auto zip = stored_zip({{"Caf\x82 Fox/read me \x82.txt", {'h', 'i'}}, {"Caf\x82 Fox/PlFxOr \x82t\x82.dat", orange}});
    std::vector<uint8_t> dat;
    bool threw = false, found = false;
    try {
      found = host::skins::costume_from_download(zip, "PlFxOr.dat", "", &dat, &why);
    } catch (...) {
      threw = true;
    }
    check(!threw && found && dat == orange, "legacy-encoded names in a download: the costume found, nothing thrown");
  }

  std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
