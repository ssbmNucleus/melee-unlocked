// costume_served_form (host/costume_served_form.h) on synthetic costume files: what is served, what is
// refused, and that a served skeleton is the retail one bit for bit.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "costume_served_form.h"

#include "cosmetic_mods.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// The cosmetic catalog (cosmetic_*.cpp, linked for its identity, structure and skeleton checks) needs these three.
namespace host {
void log(const char* format, ...) {
  va_list args; va_start(args, format); std::vfprintf(stdout, format, args); va_end(args);
  std::fputc('\n', stdout);
}
bool disc_find_file(const std::string&, uint32_t*, uint32_t*) { return false; }
bool disc_read(uint32_t, void*, uint32_t) { return false; }
}

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); ++failures; }
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = (uint8_t)(x >> 24); v[at + 1] = (uint8_t)(x >> 16); v[at + 2] = (uint8_t)(x >> 8); v[at + 3] = (uint8_t)x;
}
uint32_t get32(const std::vector<uint8_t>& v, size_t at) {
  return (uint32_t)v[at] << 24 | (uint32_t)v[at + 1] << 16 | (uint32_t)v[at + 2] << 8 | v[at + 3];
}
uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
constexpr float kPi = 3.14159265f;

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

// A joint: flags, then the rest pose (rotation, scale 1, translation), a bind matrix unless `unbound`.
uint32_t joint(Dat& d, uint32_t flags, float rx, float ty, bool unbound = false) {
  const uint32_t j = d.alloc(0x40);
  d.word(j + 4, flags);
  d.word(j + 0x14, bits(rx));
  d.word(j + 0x20, bits(1)); d.word(j + 0x24, bits(1)); d.word(j + 0x28, bits(1));
  d.word(j + 0x30, bits(ty));
  if (!unbound) { const uint32_t m = d.alloc(0x30); d.ptr(j + 0x38, m); }
  return j;
}
// A joint's rest rotation, all three angles (HSD's order: R = Rz * Ry * Rx).
void rotation(Dat& d, uint32_t j, float rx, float ry, float rz) {
  d.word(j + 0x14, bits(rx)); d.word(j + 0x18, bits(ry)); d.word(j + 0x1C, bits(rz));
}

struct Options {
  std::string fighter = "Fox", colour = "Gr";   // the symbols' slot
  float rest_delta = 0;                         // added to joint 2's translation
  bool cut = false;                             // the last leaf is missing
  bool extra_joint = false;                     // one more joint, under the last leaf (last in the game's order)
  float chain_delta = 0;                        // added to joint 4's rest rotation and translation (a physics chain's tip)
  bool reshape = false;                         // joint 3 hangs under joint 2 instead of beside it
  bool extra_skeleton_root = false, extra_data_root = false, extra_matanim_root = false;
  bool constraint = false, unbound_envelope = false, odd_flags = false;
  uint32_t joint2_flags = 0;                    // joint 2's flags when not odd_flags (a quaternion, an IK type)
  // Joints 2 and 3 rotated as two of Mario's bones are (Sunshine Mario, post 16086, joints 42 and 30):
  // 1 retail's angles; 2 the same rotations as that skin's model tool wrote them (joint 3's Z -pi as
  // pi; joint 2's X folded into Z at Y = -pi/2, where only X + Z counts), each past the tolerance.
  int euler = 0;
  bool particle_joint = false;                  // joint 3 a particle joint (0x20, a visual flag) with a particle list
  int dobjs = 2;                                // display objects on joint 1
  bool hat = false;                             // Jigglypuff's hat root
  bool hat_reshaped = false;
  uint8_t texture = 0x11;                       // a byte of "texture" data, to tell skins apart
  std::string class_name;                       // joint 2's class name (+0x00, a pointer to text); "" none
  uint32_t class_raw = 0;                       // joint 2's +0x00 as a value no relocation lists
};

// Root (0) -> 1 -> {2, 3 beside it} -> 4 under 3; a matanim root; a few texture bytes.
std::vector<uint8_t> costume(const Options& o) {
  Dat d;
  const uint32_t texture = d.alloc(0x20);
  for (uint32_t i = 0; i < 0x20; ++i) d.data[texture + i] = (uint8_t)(o.texture + i);
  const uint32_t j0 = joint(d, 0x2, 0, 0), j1 = joint(d, 0x0, 0.25f, 1),
                 j2 = joint(d, o.odd_flags ? 0x1000 : o.joint2_flags, 0, 2 + o.rest_delta);
  d.ptr(j0 + 0x08, j1);
  d.ptr(j1 + 0x08, j2);
  const uint32_t j3 = joint(d, 0, 0.5f, 3);
  if (o.euler == 1) { rotation(d, j2, -0.0228f, -kPi / 2, 3.1503f); rotation(d, j3, -kPi / 2, 0, -kPi); }
  if (o.euler == 2) { rotation(d, j2, 0, -kPi / 2, 3.1275f); rotation(d, j3, -kPi / 2, -0.0f, kPi); }
  if (o.reshape) d.ptr(j2 + 0x08, j3); else d.ptr(j2 + 0x0C, j3);
  if (!o.cut) {
    const uint32_t j4 = joint(d, 0, o.chain_delta, 4 + o.chain_delta);
    d.ptr(j3 + 0x08, j4);
    if (o.extra_joint) { const uint32_t j5 = joint(d, 0, 0, 5); d.ptr(j4 + 0x08, j5); }
  }
  if (o.constraint) { const uint32_t robj = d.alloc(0x10); d.ptr(j3 + 0x3C, robj); }
  if (!o.class_name.empty()) {
    const uint32_t text = d.alloc((uint32_t)o.class_name.size() + 1);
    std::memcpy(&d.data[text], o.class_name.c_str(), o.class_name.size() + 1);
    d.ptr(j2, text);
  } else if (o.class_raw) {
    d.word(j2, o.class_raw);
  }
  if (o.particle_joint) {
    d.word(j3 + 4, 0x20);
    const uint32_t list = d.alloc(0x10);
    d.ptr(j3 + 0x10, list);
  }
  // Display objects on joint 1: DObj { name, next, mobj, pobj }.
  uint32_t previous = 0;
  for (int i = 0; i < o.dobjs; ++i) {
    const uint32_t dobj = d.alloc(0x10);
    if (i == 0) d.ptr(j1 + 0x10, dobj); else d.ptr(previous + 0x04, dobj);
    previous = dobj;
    if (i == 0 && o.unbound_envelope) {
      // An envelope mesh blending joint 4's sibling-less leaf and an unbound joint.
      const uint32_t pobj = d.alloc(0x18);
      d.ptr(dobj + 0x0C, pobj);
      d.data[pobj + 0x0C] = 0x20;   // envelope type (0x2000)
      const uint32_t list = d.alloc(8), weights = d.alloc(24);
      d.ptr(pobj + 0x14, list);
      d.ptr(list, weights);
      const uint32_t loose = joint(d, 0, 0, 0, true);
      d.ptr(weights, j2); d.word(weights + 4, bits(0.5f));
      d.ptr(weights + 8, loose); d.word(weights + 12, bits(0.5f));
    }
  }
  const std::string colour = o.colour == "Nr" ? "" : o.colour;
  d.root(j0, "Ply" + o.fighter + "5K" + colour + "_Share_joint");
  const uint32_t matanim = d.alloc(0x20);
  d.root(matanim, "Ply" + o.fighter + "5K" + colour + "_Share_matanim_joint");
  if (o.hat) {
    const uint32_t h0 = joint(d, 0x2, 0, 0), h1 = joint(d, 0, 0, 0.75f);
    if (o.hat_reshaped) d.ptr(h0 + 0x0C, h1); else d.ptr(h0 + 0x08, h1);
    d.root(h0, "Ply" + o.fighter + colour + "Hat_TopN_joint");
  }
  if (o.extra_skeleton_root) d.root(j3, "Ply" + o.fighter + "5K" + colour + "_Extra_joint");
  if (o.extra_data_root) { const uint32_t mex = d.alloc(0x10); d.root(mex, "mexCostume"); }
  if (o.extra_matanim_root) d.root(matanim, "Ply" + o.fighter + "5K" + colour + "_SharX_matanim_joint");
  return d.build();
}

void expect(const host::skins::ServedForm& r, bool ok, const std::string& reason_part, const std::string& name) {
  check(r.ok == ok, name + ": ok " + (r.ok ? "true" : "false") + " (" + r.reason + ")");
  if (!reason_part.empty()) check(r.reason.find(reason_part) != std::string::npos, name + ": reason \"" + r.reason + "\" lacks \"" + reason_part + "\"");
  if (r.ok) check(!r.bytes.empty(), name + ": no bytes served");
  else check(r.bytes.empty(), name + ": bytes served on refusal");
  std::printf("%-34s %s: %s\n", name.c_str(), r.ok ? "served" : "refused", r.reason.c_str());
}

bool has_symbol(const std::vector<uint8_t>& f, const std::string& symbol) {
  return std::search(f.begin(), f.end(), symbol.begin(), symbol.end()) != f.end();
}

}  // namespace

int main() {
  using host::skins::costume_served_form;
  using host::skins::ServedFormPolicy;
  const auto retail = costume({});
  const std::string slot = "PlFxGr.dat";

  {
    Options o; o.texture = 0x77;
    const auto skin = costume(o);
    const auto r = costume_served_form(retail, skin, slot);
    expect(r, true, "5 joints in 1 skeleton roots", "same slot, new textures");
    check(r.bytes == skin, "same slot: served bytes are the skin's own");
    check(r.snapped == 0, "same slot: nothing snapped");
    check(r.source_slot == "PlFxGr.dat", "same slot: source slot");
  }
  {
    Options o; o.colour = "Or"; o.texture = 0x55;
    const auto r = costume_served_form(retail, costume(o), slot);
    expect(r, true, "", "renamed from Orange");
    check(has_symbol(r.bytes, "PlyFox5KGr_Share_joint") && has_symbol(r.bytes, "PlyFox5KGr_Share_matanim_joint") &&
              !has_symbol(r.bytes, "PlyFox5KOr_"), "renamed: symbols are the target slot's");
    check(r.source_slot == "PlFxOr.dat", "renamed: source slot");
    check(host::cosmetics::testing::inspect_dat(r.bytes).target_path == "PlFxGr.dat", "renamed: identifies as the target");
  }
  {
    Options o; o.colour = "Nr";
    expect(costume_served_form(retail, costume(o), slot), true, "", "renamed from the default colour");
  }
  {
    Options o; o.rest_delta = 0.0005f;
    const auto skin = costume(o);
    const auto r = costume_served_form(retail, skin, slot);
    expect(r, true, "1 rest poses replaced", "rest pose within tolerance");
    std::string why;
    check(host::skins::skeleton_identical(retail, r.bytes, &why), "within tolerance: skeleton is retail's: " + why);
    check(!host::skins::skeleton_identical(retail, skin, nullptr), "within tolerance: the skin itself was not");
  }
  {
    Options o; o.rest_delta = 0.5f;
    const auto skin = costume(o);
    expect(costume_served_form(retail, skin, slot), false, "rest pose differs", "rest pose beyond tolerance");
    ServedFormPolicy snap; snap.snap_any_rest_pose = true;
    const auto r = costume_served_form(retail, skin, slot, snap);
    expect(r, true, "1 rest poses replaced", "beyond tolerance, snap any size");
    check(host::skins::skeleton_identical(retail, r.bytes, nullptr), "snap any size: skeleton is retail's");
    // Only the rest pose changed: everything else is the skin's.
    size_t differing = 0;
    for (size_t i = 0; i < skin.size(); ++i) differing += skin[i] != r.bytes[i];
    check(r.bytes.size() == skin.size() && differing > 0 && differing <= 4, "snap any size: only the rest pose bytes change");
  }
  { Options o; o.cut = true; expect(costume_served_form(retail, costume(o), slot), false, "joint count differs", "cut skeleton (L1)"); }
  { Options o; o.extra_skeleton_root = true; expect(costume_served_form(retail, costume(o), slot), false, "unknown skeleton root", "unknown skeleton symbol (L2)"); }
  { Options o; o.extra_matanim_root = true; expect(costume_served_form(retail, costume(o), slot), false, "unknown joint root", "unknown matanim joint root"); }
  { Options o; o.extra_data_root = true; o.colour = "La";
    const auto r = costume_served_form(retail, costume(o), slot);
    expect(r, true, "", "extra data root (mexCostume)");
    check(has_symbol(r.bytes, "mexCostume"), "extra data root: kept, unread"); }
  { Options o; o.reshape = true; expect(costume_served_form(retail, costume(o), slot), false, "hierarchy differs", "shape mismatch (L3)"); }
  { Options o; o.odd_flags = true; expect(costume_served_form(retail, costume(o), slot), false, "flags differ", "non-visual joint flags"); }
  { Options o; o.constraint = true; expect(costume_served_form(retail, costume(o), slot), false, "constraint", "joint constraint"); }
  // The importer's structure rule (mod_data_safety.h): the particle flag is a visual flag, so the skeleton
  // walk lets it through, but the importer refuses a particle joint with a particle list; so does serving.
  { Options o; o.particle_joint = true;
    expect(costume_served_form(retail, costume(o), slot), false, "particle joint", "particle joint (importer rule)"); }
  { Options o; o.unbound_envelope = true;
    // The retail file must carry the same extra joint for the trees to match; the loose joint is not in the tree.
    expect(costume_served_form(retail, costume(o), slot), false, "bind matrix", "envelope on an unbound joint"); }
  {
    Options o; o.dobjs = 124;
    const auto many = costume(o);
    expect(costume_served_form(retail, many, slot), true, "", "124 display objects");
    o.dobjs = 125;
    expect(costume_served_form(retail, costume(o), slot), false, "display objects", "125 display objects");
  }
  { Options o; o.fighter = "Falco"; o.colour = "Gr"; expect(costume_served_form(retail, costume(o), slot), false, "not a Fox costume", "another fighter's skin"); }
  { expect(costume_served_form(retail, costume({}), "PlFxOr.dat"), false, "not the retail PlFxOr.dat", "reference for another slot"); }
  {
    Options o; o.colour = "Bk";   // m-ex's extra colours: Fox has no black on the retail disc
    expect(costume_served_form(retail, costume(o), slot), false, "no retail slot", "extra colour source (default)");
    ServedFormPolicy extra; extra.allow_extra_colour_source = true;
    expect(costume_served_form(retail, costume(o), slot, extra), true, "", "extra colour source (allowed)");
  }
  {
    auto skin = costume({});
    skin.resize(skin.size() - 3);
    expect(costume_served_form(retail, skin, slot), false, "structure", "truncated file");
    expect(costume_served_form(retail, {}, slot), false, "structure", "empty file");
    auto lying = costume({});
    put32(lying, 4, 0x7FFFFFF0);   // data block size past the file
    expect(costume_served_form(retail, lying, slot), false, "structure", "data size past the file");
  }
  {
    // Jigglypuff: the hat is a skeleton root of its own and is checked like the body.
    Options re; re.fighter = "Purin"; re.colour = "Re"; re.hat = true;
    const auto retail_re = costume(re);
    Options bu = re; bu.colour = "Bu"; bu.texture = 0x33;
    const auto r = costume_served_form(retail_re, costume(bu), "PlPrRe.dat");
    expect(r, true, "2 skeleton roots", "Jigglypuff Blue skin on Red");
    check(has_symbol(r.bytes, "PlyPurinReHat_TopN_joint") && !has_symbol(r.bytes, "PlyPurinBuHat_"), "Jigglypuff: hat renamed");
    Options hatless = re; hatless.hat = false;
    expect(costume_served_form(retail_re, costume(hatless), "PlPrRe.dat"), false, "missing root PlyPurinReHat_TopN_joint", "Jigglypuff skin without a hat");
    Options odd_hat = re; odd_hat.hat_reshaped = true;
    expect(costume_served_form(retail_re, costume(odd_hat), "PlPrRe.dat"), false, "PlyPurinReHat_TopN_joint:", "Jigglypuff hat of another shape");
    Options nr = re; nr.colour = "Nr"; nr.hat = false;
    expect(costume_served_form(retail_re, costume(nr), "PlPrRe.dat"), false, "missing root PlyPurinReHat_TopN_joint", "Jigglypuff default (no hat) on Red");
  }
  {
    // Offline (costume_offline_form): the strict form when it passes, else the skin as installed when the
    // game can load it; online stays costume_served_form.
    using host::skins::costume_offline_form;
    Options o; o.rest_delta = 0.0005f;
    expect(costume_offline_form(retail, costume(o), slot), true, "1 rest poses replaced", "offline: within tolerance, strict form");
    o = {}; o.extra_joint = true;
    const auto more = costume(o);
    expect(costume_served_form(retail, more, slot), false, "joint count differs", "one joint more: no online form");
    expect(costume_served_form(retail, more, slot, host::skins::own_skin_policy()), false, "joint count differs",
           "one joint more: no online form (own rule)");
    const auto r = costume_offline_form(retail, more, slot);
    expect(r, true, "6 joints, the retail costume 5", "offline: one joint more, as installed");
    check(r.bytes == more, "offline: one joint more is served byte for byte");
    o = {}; o.cut = true;
    expect(costume_offline_form(retail, costume(o), slot), false, "fewer than the fighter's 5", "offline: one joint fewer (the game stops)");
    o = {}; o.colour = "Or";
    expect(costume_offline_form(retail, costume(o), slot), true, "", "offline: another colour's skin, strict form renamed");
    o = {}; o.colour = "Or"; o.extra_joint = true;
    expect(costume_offline_form(retail, costume(o), slot), false, "made for PlFxOr.dat", "offline: as installed only in its own slot");
    o = {}; o.particle_joint = true;
    expect(costume_offline_form(retail, costume(o), slot), false, "particle joint", "offline: particle list (memory safety)");
    o = {}; o.unbound_envelope = true;
    expect(costume_offline_form(retail, costume(o), slot), false, "bind matrix", "offline: unbound envelope (the game stops)");
    o = {}; o.extra_joint = true; o.dobjs = 125;
    expect(costume_offline_form(retail, costume(o), slot), false, "125 meshes", "offline: 125 listed meshes (the game stops)");
    o = {}; o.odd_flags = true; o.constraint = true;
    expect(costume_offline_form(retail, costume(o), slot), true, "as installed", "offline: odd flags and a constraint load");
  }
  {
    // A physics chain's bones (Marth's cape or hair worn as another model's hair): nothing looks at them
    // apart from the rest of the skeleton. Moved past the tolerance, the skin has no strict form; online
    // (the own rule, which is the peer rule) the chain gets the retail rest pose, as the desktop app's
    // own fix does; offline it keeps its own.
    Options o; o.chain_delta = 0.4f;
    const auto hair = costume(o);
    expect(costume_served_form(retail, hair, slot), false, "Skeleton joint 4 rest pose differs", "physics chain moved: strict");
    const auto online = costume_served_form(retail, hair, slot, host::skins::own_skin_policy());
    expect(online, true, "1 rest poses replaced", "physics chain moved: online (own rule)");
    check(host::skins::skeleton_identical(retail, online.bytes, nullptr), "physics chain online: the retail rest pose");
    const auto offline = host::skins::costume_offline_form(retail, hair, slot);
    expect(offline, true, "as installed", "physics chain moved: offline");
    check(offline.bytes == hair, "physics chain offline: its own rest pose, byte for byte");
  }
  {
    // An opponent's skin (peer_skin_policy): a rest pose never refuses, since every joint gets the retail
    // one (proven bit for bit) before the game sees it; whatever else the simulation reads stays strict.
    using host::skins::peer_skin_policy;
    Options re; re.euler = 1;
    const auto retail_euler = costume(re);
    Options sk = re; sk.euler = 2; sk.texture = 0x44;
    const auto sunshine = costume(sk);
    // The rule before 2026-11-25 (the importer's tolerance) refused it: "Skeleton joint 30 rest pose
    // differs from the vanilla costume (rotated)" on the real file. Offline still takes that path first.
    expect(costume_served_form(retail_euler, sunshine, slot), false, "rest pose differs", "Sunshine pattern: importer's tolerance");
    const auto r = costume_served_form(retail_euler, sunshine, slot, peer_skin_policy());
    expect(r, true, "2 rest poses replaced (2 past the importer's tolerance, joints 2, 3: the same placement written as other angles)",
           "Sunshine pattern: peer rule");
    check(r.past_tolerance == 2 && r.placed_elsewhere == 0, "Sunshine pattern: two joints past the tolerance, both the same placement");
    std::string why;
    check(host::skins::skeleton_identical(retail_euler, r.bytes, &why), "Sunshine pattern: skeleton is retail's: " + why);
    size_t differing = 0;
    for (size_t i = 0; i < sunshine.size(); ++i) differing += sunshine[i] != r.bytes[i];
    check(r.bytes.size() == sunshine.size() && differing > 0 && differing <= 4 * 4, "Sunshine pattern: only the rest pose bytes change");

    Options moved; moved.rest_delta = 0.5f;
    const auto elsewhere = costume_served_form(retail, costume(moved), slot, peer_skin_policy());
    expect(elsewhere, true, "1 past the importer's tolerance, joint 2: 1 placed elsewhere, drawn on the retail bones",
           "bone placed elsewhere: peer rule");
    check(host::skins::skeleton_identical(retail, elsewhere.bytes, nullptr), "placed elsewhere: skeleton is retail's");

    // What the simulation reads beyond the rest pose: each still refuses, so the opponent sees the retail costume.
    const std::pair<const char*, std::function<void(Options&)>> refused[] = {
        {"joint count differs", [](Options& o) { o.cut = true; }},
        {"joint count differs", [](Options& o) { o.extra_joint = true; }},
        {"hierarchy differs", [](Options& o) { o.reshape = true; }},
        {"flags differ", [](Options& o) { o.odd_flags = true; }},
        {"flags differ", [](Options& o) { o.joint2_flags = 1u << 17; }},   // a quaternion rotation
        {"flags differ", [](Options& o) { o.joint2_flags = 3u << 21; }},   // an IK effector
        {"constraint", [](Options& o) { o.constraint = true; }},
        {"class name differs", [](Options& o) { o.class_name = "jobj"; }},
        {"class name differs", [](Options& o) { o.class_raw = 0x1234; }},
        {"unknown skeleton root", [](Options& o) { o.extra_skeleton_root = true; }},
        {"no retail slot", [](Options& o) { o.colour = "Bk"; }},
        {"bind matrix", [](Options& o) { o.unbound_envelope = true; }},
    };
    int n = 0;
    for (const auto& c : refused) {
      Options o = sk;   // the Sunshine pattern too: a rest pose difference does not hide the rest
      c.second(o);
      expect(costume_served_form(retail_euler, costume(o), slot, peer_skin_policy()), false, c.first,
             "peer rule refuses " + std::to_string(++n) + " (" + c.first + ")");
    }
    Options purin; purin.fighter = "Purin"; purin.colour = "Re"; purin.hat = true;
    Options hatless = purin; hatless.hat = false;
    expect(costume_served_form(costume(purin), costume(hatless), "PlPrRe.dat", peer_skin_policy()), false,
           "missing root PlyPurinReHat_TopN_joint", "peer rule refuses a missing hat root");
  }
  {
    // The wearer's own machine (wearer_form: what the costume slots hand the player's own port). Online
    // the peer rule itself (2026-11-25): a non-visual flag or a constraint, which an opponent's machine
    // refuses, is the retail costume on the wearer's side too; offline the skin as installed, as before.
    // The disc-slot rule (a mod disc, a pack, an L / R pick: cosmetic_mods.h costume_online_form) refuses
    // them too.
    using host::skins::wearer_form;
    const std::pair<const char*, std::function<void(Options&)>> sim[] = {
        {"flags differ", [](Options& o) { o.odd_flags = true; }},
        {"flags differ", [](Options& o) { o.joint2_flags = 1u << 17; }},   // a quaternion rotation
        {"constraint", [](Options& o) { o.constraint = true; }},
    };
    for (const auto& c : sim) {
      Options o; o.texture = 0x66; c.second(o);
      const auto skin = costume(o);
      const std::string what = std::string(" (") + c.first + ")";
      expect(wearer_form(retail, skin, slot, true), false, c.first, "wearer online: retail" + what);
      const auto offline = wearer_form(retail, skin, slot, false);
      expect(offline, true, "as installed", "wearer offline: the skin" + what);
      check(offline.bytes == skin, "wearer offline: the skin byte for byte" + what);
      std::vector<uint8_t> fixed;
      std::string detail;
      check(!host::cosmetics::costume_online_form(retail, skin, &fixed, &detail) && detail.find(c.first) != std::string::npos,
            "disc slot online: retail" + what + ": " + detail);
    }
    // The joint's class name (+0x00): HSD_JObjLoadJoint builds a joint whose description names a
    // registered class as that class. Compared online (both no name, or the same text); offline unchanged.
    Options named; named.class_name = "jobj"; named.texture = 0x22;
    expect(wearer_form(retail, costume(named), slot, true), false, "class name differs", "wearer online: a class name retail lacks");
    expect(wearer_form(retail, costume(named), slot, false), true, "", "wearer offline: a class name, the strict form as before");
    Options named_retail; named_retail.class_name = "jobj";
    const auto retail_named = costume(named_retail);
    expect(wearer_form(retail_named, costume(named), slot, true), true, "", "wearer online: the same class name as retail");
    Options other_name = named; other_name.class_name = "jobx";
    expect(wearer_form(retail_named, costume(other_name), slot, true), false, "class name differs", "wearer online: another class name");
  }
  {
    // The invariant (character-skins.md): online, a skin is either shown on both machines on a skeleton
    // bit-equal to retail, or on neither. For every fixture: the wearer's online form and the form an
    // opponent's machine builds (the peer rule) give the same verdict and the same bytes, and a served
    // form is the retail skeleton in all the simulation reads (skeleton_identical). The disc-slot rule
    // gives the same verdict, and the same bytes, for a skin the importer takes in the slot it was made for.
    using host::skins::peer_skin_policy;
    using host::skins::wearer_form;
    Options euler_base; euler_base.euler = 1;
    const auto retail_euler = costume(euler_base);
    struct Case { std::string name; const std::vector<uint8_t>* retail; Options skin; };
    std::vector<Case> cases;
    auto add = [&](const std::string& name, const std::function<void(Options&)>& f, bool euler = false) {
      Options o = euler ? euler_base : Options{};
      o.texture = 0x5A;
      if (euler) o.euler = 2;
      f(o);
      cases.push_back({name, euler ? &retail_euler : &retail, o});
    };
    add("new textures", [](Options&) {});
    add("made for orange", [](Options& o) { o.colour = "Or"; });
    add("made for the default colour", [](Options& o) { o.colour = "Nr"; });
    add("rest pose within tolerance", [](Options& o) { o.rest_delta = 0.0005f; });
    add("bone placed elsewhere", [](Options& o) { o.rest_delta = 0.5f; });
    add("physics chain moved", [](Options& o) { o.chain_delta = 0.4f; });
    add("one joint fewer", [](Options& o) { o.cut = true; });
    add("one joint more", [](Options& o) { o.extra_joint = true; });
    add("another shape", [](Options& o) { o.reshape = true; });
    add("non-visual flag", [](Options& o) { o.odd_flags = true; });
    add("quaternion", [](Options& o) { o.joint2_flags = 1u << 17; });
    add("IK effector", [](Options& o) { o.joint2_flags = 3u << 21; });
    add("visual flag only", [](Options& o) { o.joint2_flags = 0x8; });
    add("constraint", [](Options& o) { o.constraint = true; });
    add("class name", [](Options& o) { o.class_name = "jobj"; });
    add("class name slot not relocated", [](Options& o) { o.class_raw = 0x1234; });
    add("unknown skeleton root", [](Options& o) { o.extra_skeleton_root = true; });
    add("unknown matanim root", [](Options& o) { o.extra_matanim_root = true; });
    add("mexCostume root", [](Options& o) { o.extra_data_root = true; });
    add("m-ex extra colour", [](Options& o) { o.colour = "Bk"; });
    add("unbound envelope", [](Options& o) { o.unbound_envelope = true; });
    add("particle joint", [](Options& o) { o.particle_joint = true; });
    add("125 display objects", [](Options& o) { o.dobjs = 125; });
    add("Sunshine pattern", [](Options&) {}, true);
    add("Sunshine pattern, quaternion", [](Options& o) { o.joint2_flags = 1u << 17; }, true);
    add("Sunshine pattern, constraint", [](Options& o) { o.constraint = true; }, true);
    int shown = 0, neither = 0, disc_compared = 0;
    for (const auto& c : cases) {
      const auto skin = costume(c.skin);
      const auto own = wearer_form(*c.retail, skin, slot, true);
      const auto peer = costume_served_form(*c.retail, skin, slot, peer_skin_policy());
      check(own.ok == peer.ok && own.bytes == peer.bytes, "invariant: own and peer agree: " + c.name + " (own: " + own.reason +
                                                              "; peer: " + peer.reason + ")");
      std::string why;
      if (own.ok) check(host::skins::skeleton_identical(*c.retail, own.bytes, &why), "invariant: served on the retail skeleton: " + c.name + " " + why);
      (own.ok ? shown : neither) += 1;
      std::string unsafe;
      if (c.skin.colour != "Gr" || !host::cosmetics::testing::costume_structure_safe(skin, c.retail, &unsafe)) continue;
      ++disc_compared;
      std::vector<uint8_t> fixed;
      std::string detail;
      const bool disc = host::cosmetics::costume_online_form(*c.retail, skin, &fixed, &detail);
      check(disc == peer.ok && (!disc || fixed == peer.bytes), "invariant: the disc-slot rule agrees: " + c.name + " (disc: " +
                                                                detail + "; peer: " + peer.reason + ")");
    }
    std::printf("invariant: %zu fixtures, %d shown on both machines, %d on neither; disc-slot rule compared on %d\n",
                cases.size(), shown, neither, disc_compared);
  }
  std::printf("%s\n", failures ? "costume_served_form: FAILED" : "costume_served_form: all passed");
  return failures ? 1 : 0;
}
