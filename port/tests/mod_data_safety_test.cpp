// SPDX-License-Identifier: GPL-2.0-or-later
// mod_data_safety.h on minimal files: each check takes a well-formed file and refuses the same file
// with one value past its limit (a count one too many, an offset past the file, a loop).
#include "mod_data_safety.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace host::cosmetics::data_safety;

int failed = 0;
void check(bool ok, const char* name, const std::string& why = std::string()) {
  if (!ok) { std::fprintf(stderr, "FAIL %s (%s)\n", name, why.c_str()); ++failed; }
}

// A DAT archive built field by field: the data block, its listed pointer fields and its roots.
struct Dat {
  std::vector<uint8_t> data;
  std::vector<uint32_t> pointers;
  std::vector<std::pair<std::string, uint32_t>> roots;
  uint32_t alloc(uint32_t bytes) {
    const uint32_t at = uint32_t((data.size() + 31) & ~size_t(31));
    data.resize(at + bytes, 0);
    return at;
  }
  void word(uint32_t at, uint32_t v) {
    data[at] = uint8_t(v >> 24); data[at + 1] = uint8_t(v >> 16); data[at + 2] = uint8_t(v >> 8); data[at + 3] = uint8_t(v);
  }
  void half(uint32_t at, uint16_t v) { data[at] = uint8_t(v >> 8); data[at + 1] = uint8_t(v); }
  void ptr(uint32_t at, uint32_t target) {
    word(at, target);
    for (uint32_t p : pointers) if (p == at) return;
    pointers.push_back(at);
  }
  void unlist(uint32_t at) {
    for (size_t i = 0; i < pointers.size(); ++i) if (pointers[i] == at) { pointers.erase(pointers.begin() + i); return; }
  }
  std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> out(32, 0);
    out.insert(out.end(), data.begin(), data.end());
    auto put = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
    for (uint32_t p : pointers) put(p);
    uint32_t name = 0;
    for (const auto& r : roots) { put(r.second); put(name); name += uint32_t(r.first.size() + 1); }
    for (const auto& r : roots) { out.insert(out.end(), r.first.begin(), r.first.end()); out.push_back(0); }
    auto set = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (24 - 8 * i)); };
    set(0, uint32_t(out.size())); set(4, uint32_t(data.size())); set(8, uint32_t(pointers.size()));
    set(12, uint32_t(roots.size()));
    return out;
  }
};

// Runs `test` on the file as an Archive. Every fixture parses, so a refusal is the check's own.
template <typename F> bool on(const Dat& d, F test) {
  const std::vector<uint8_t> bytes = d.bytes();
  Archive a(bytes);
  if (!a.parse()) { check(false, "a fixture parses as an archive"); return false; }
  return test(a);
}

// A collision with two vertices, one line between them and one joint holding both.
struct Coll { Dat d; uint32_t coll, verts, lines, joints; };
Coll collision() {
  Coll c;
  c.coll = c.d.alloc(0x30);
  c.verts = c.d.alloc(2 * 8);
  c.joints = c.d.alloc(0x28);
  c.lines = c.d.alloc(16);   // last, so a line past the count lies past the file
  c.d.ptr(c.coll, c.verts); c.d.word(c.coll + 4, 2);
  c.d.ptr(c.coll + 8, c.lines); c.d.word(c.coll + 12, 1);
  c.d.half(c.coll + 0x12, 1);                      // group 0: lines 0..1
  c.d.ptr(c.coll + 0x24, c.joints); c.d.word(c.coll + 0x28, 1);
  c.d.half(c.joints + 2, 1);                       // the joint's group 0: line 0
  c.d.half(c.joints + 0x26, 2);                    // the joint's vertices 0..2
  c.d.half(c.lines, 0); c.d.half(c.lines + 2, 1);
  for (uint32_t k = 0; k < 4; ++k) c.d.half(c.lines + 4 + k * 2, 0xFFFF);
  c.d.roots.push_back({"coll_data", c.coll});
  return c;
}
bool coll_ok(const Coll& c, std::string* why) {
  return on(c.d, [&](const Archive& a) { return collision_valid(a, c.coll, why); });
}

void test_collision() {
  std::string why;
  Coll c = collision();
  check(coll_ok(c, &why), "a whole collision passes", why);
  c = collision(); c.d.word(c.coll + 4, 2049);
  check(!coll_ok(c, &why), "2049 collision vertices are refused");
  c = collision(); c.d.word(c.coll + 12, 1537);
  check(!coll_ok(c, &why), "1537 collision lines are refused");
  c = collision(); c.d.word(c.coll + 0x28, 0);
  check(!coll_ok(c, &why), "a collision with no joint is refused");
  c = collision(); c.d.word(c.coll + 0x28, 257);
  check(!coll_ok(c, &why), "257 collision joints are refused");
  c = collision(); c.d.word(c.coll + 12, 2);
  check(!coll_ok(c, &why), "a line array one line short of its count is refused");
  c = collision(); c.d.half(c.coll + 0x12, 2);
  check(!coll_ok(c, &why), "a line group one past the lines is refused");
  c = collision(); c.d.half(c.joints + 0x26, 3);
  check(!coll_ok(c, &why), "a joint's vertices one past the vertices are refused");
  c = collision(); c.d.half(c.lines + 2, 2);
  check(!coll_ok(c, &why), "a line naming vertex 2 of 2 is refused");
  c = collision(); c.d.half(c.lines + 4, 1);
  check(!coll_ok(c, &why), "a line naming neighbour 1 of 1 is refused");
}

// A texture bank: one group of one 8x8 I4 texture (32 bytes).
struct Tex { Dat d; uint32_t bank, group, image; };
Tex textures(uint32_t width = 8) {
  Tex t;
  t.d.alloc(32);   // so the bank is not at offset 0, which reads as "no bank" in an effect table
  t.bank = t.d.alloc(8);
  t.group = t.d.alloc(0x18 + 4);
  t.image = t.d.alloc(32);
  t.d.word(t.bank, 1); t.d.word(t.bank + 4, t.group - t.bank);
  t.d.word(t.group, 1); t.d.word(t.group + 0xC, width); t.d.word(t.group + 0x10, 8);
  t.d.word(t.group + 0x18, t.image - t.bank);
  t.d.roots.push_back({"map_texg", t.bank});
  return t;
}
bool tex_ok(const Tex& t, std::string* why) {
  return on(t.d, [&](const Archive& a) { return texture_bank_valid(a, t.bank, why); });
}

void test_textures() {
  std::string why;
  Tex t = textures();
  check(tex_ok(t, &why), "a whole texture bank passes", why);
  t = textures(); t.d.word(t.bank, 0x10000);
  check(!tex_ok(t, &why), "a texture group count past the file is refused");
  t = textures(); t.d.word(t.bank + 4, uint32_t(t.d.data.size()));
  check(!tex_ok(t, &why), "a texture group past the file is refused");
  t = textures(); t.d.word(t.group + 0x18, uint32_t(t.d.data.size() - t.bank));
  check(!tex_ok(t, &why), "a texture past the file is refused");
  t = textures(16);
  check(!tex_ok(t, &why), "a texture whose pixels run past the file is refused");
  t = textures(); t.d.word(t.group + 4, 9); t.d.word(t.group + 0x14, 257u << 16);
  check(!tex_ok(t, &why), "a texture group with 257 palettes is refused");
  const Tex a = textures(), b = textures(), wide = textures(16);
  const std::vector<uint8_t> ab = a.d.bytes(), bb = b.d.bytes(), wb = wide.d.bytes();
  Archive aa(ab), ba(bb), wa(wb);
  check(aa.parse() && ba.parse() && wa.parse(), "the texture files parse");
  check(texture_banks_same_shape(aa, a.bank, ba, b.bank, &why), "two texture banks of one shape match", why);
  check(!texture_banks_same_shape(aa, a.bank, wa, wide.bank, &why), "a texture bank of another size is refused");
}

// A command bank, version 0: one command list (0x40 bytes).
struct Cmd { Dat d; uint32_t bank, list; };
Cmd commands() {
  Cmd c;
  c.bank = c.d.alloc(12);
  c.list = c.d.alloc(0x40);
  c.d.word(c.bank + 4, 1); c.d.word(c.bank + 8, c.list - c.bank);
  c.d.roots.push_back({"map_ptcl", c.bank});
  return c;
}
bool cmd_ok(const Cmd& c, std::string* why) {
  return on(c.d, [&](const Archive& a) { return command_bank_valid(a, c.bank, why); });
}

void test_commands() {
  std::string why;
  Cmd c = commands();
  check(cmd_ok(c, &why), "a whole command bank passes", why);
  c = commands(); c.d.word(c.bank + 4, 0x10000);
  check(!cmd_ok(c, &why), "a command list count past the file is refused");
  c = commands(); c.d.word(c.bank + 8, uint32_t(c.d.data.size() - c.bank - 0x3C));
  check(!cmd_ok(c, &why), "a command list running past the file is refused");
  c = commands(); c.d.word(c.bank, 0x00440000);
  check(!cmd_ok(c, &why), "a command bank of an unknown version is refused");
}

// Joints: a root with one child; `flags` on the child.
struct Joints { Dat d; uint32_t root, child; };
Joints joints(uint32_t flags = 0) {
  Joints j;
  j.root = j.d.alloc(0x40);
  j.child = j.d.alloc(0x40);
  j.d.ptr(j.root + 8, j.child);
  j.d.word(j.child + 4, flags);
  if (flags & 0x20) j.d.ptr(j.child + 0x10, j.d.alloc(8));   // a particle joint's list
  return j;
}
bool plain(const Joints& j, bool splines, std::string* why, uint32_t* count = nullptr) {
  return on(j.d, [&](const Archive& a) { return joint_tree_plain(a, j.root, splines, why, count); });
}

void test_joints() {
  std::string why;
  uint32_t count = 0;
  Joints j = joints();
  check(plain(j, false, &why, &count) && count == 2, "a plain joint tree passes", why);
  j = joints(0x20);
  check(!plain(j, true, &why), "a particle joint with a particle list is refused");
  j.d.unlist(j.child + 0x10); j.d.word(j.child + 0x10, 0);
  check(plain(j, false, &why), "a particle joint with no particle list passes", why);
  j = joints(0x4000);
  check(!plain(j, true, &why), "a spline joint with no listed spline is refused");
  const uint32_t spline = j.d.alloc(0x20);
  j.d.ptr(j.child + 0x10, spline);
  check(plain(j, true, &why), "a stage's spline joint with a listed spline passes", why);
  check(!plain(j, false, &why), "a spline joint is refused where splines are not allowed");
  j = joints(); j.d.ptr(j.child + 0x0C, j.root);
  check(!plain(j, false, &why), "a joint tree that loops is refused");
  j = joints(0x1000); j.d.ptr(j.child + 8, j.root);
  check(plain(j, false, &why), "an instance joint's child is not followed, as the game does not", why);
}

// A costume: a two-joint skeleton, the root with `meshes` meshes.
Dat costume(uint32_t meshes) {
  Joints j = joints();
  uint32_t previous = 0;
  for (uint32_t m = 0; m < meshes; ++m) {
    const uint32_t dobj = j.d.alloc(0x10);
    j.d.ptr(m ? previous + 4 : j.root + 0x10, dobj);
    previous = dobj;
  }
  j.d.roots.push_back({"PlyFox5K_Share_joint", j.root});
  return j.d;
}
bool costume_ok(const Dat& d, uint32_t standard, std::string* why) {
  return on(d, [&](const Archive& a) { return costume_structure_valid(a, standard, why); });
}

void test_costume() {
  std::string why;
  check(costume_ok(costume(124), 2, &why), "a costume of 124 meshes and the standard's 2 joints passes", why);
  check(!costume_ok(costume(125), 2, &why), "a costume of 125 meshes is refused");
  check(!costume_ok(costume(1), 3, &why), "a costume with fewer joints than the standard is refused");
  check(costume_ok(costume(1), 1, &why), "a costume with more joints than the standard passes (they get no part)", why);
  check(costume_ok(costume(1), 0, &why), "with no standard costume, any joint count passes", why);
  {
    // 130 meshes on the second joint: past the fighter's list when that joint gets a part, unread when not.
    Joints j = joints();
    uint32_t previous = 0;
    for (uint32_t m = 0; m < 130; ++m) {
      const uint32_t dobj = j.d.alloc(0x10);
      j.d.ptr(m ? previous + 4 : j.child + 0x10, dobj);
      previous = dobj;
    }
    j.d.roots.push_back({"PlyFox5K_Share_joint", j.root});
    check(!costume_ok(j.d, 2, &why), "130 meshes on a joint that gets a part are refused");
    check(costume_ok(j.d, 1, &why), "130 meshes on a joint past the fighter's parts are not counted", why);
  }
  {
    Joints j = joints(0x4000);
    j.d.ptr(j.child + 0x10, j.d.alloc(0x20));
    j.d.roots.push_back({"PlyFox5K_Share_joint", j.root});
    check(costume_ok(j.d, 2, &why), "a costume's spline joint with a listed spline passes, as a stage's does", why);
    Joints p = joints(0x20);
    p.d.roots.push_back({"PlyFox5K_Share_joint", p.root});
    check(!costume_ok(p.d, 2, &why), "a costume's particle joint with a particle list is refused");
  }
  const Dat d = costume(1);
  const std::vector<uint8_t> bytes = d.bytes();
  Archive a(bytes);
  check(a.parse() && costume_joint_count(a) == 2, "a standard costume's joints are counted");
}

// A stage: map_head listing one model, its joint tree `flags` on the child; and a collision.
Dat stage(uint32_t flags, bool bad_collision) {
  Coll c = collision();
  if (bad_collision) c.d.word(c.coll + 0x28, 0);
  Dat& d = c.d;
  const uint32_t head = d.alloc(0x10), models = d.alloc(0x34), root = d.alloc(0x40), child = d.alloc(0x40);
  d.ptr(head + 8, models); d.word(head + 0x0C, 1);
  d.ptr(models, root); d.ptr(root + 8, child); d.word(child + 4, flags);
  if (flags & 0x20) d.ptr(child + 0x10, d.alloc(8));
  d.roots.push_back({"map_head", head});
  return d;
}

void test_stage() {
  std::string why;
  auto ok = [&](const Dat& d) { return on(d, [&](const Archive& a) { return stage_structure_valid(a, &why); }); };
  check(ok(stage(0, false)), "a whole stage passes", why);
  check(!ok(stage(0x20, false)), "a stage model with a particle joint is refused");
  check(!ok(stage(0, true)), "a stage with a collision of no joints is refused");
  Dat d = stage(0, false);
  const uint32_t head = d.roots.back().second;
  d.word(head + 0x0C, 1000);
  check(!ok(d), "a model list running past the file is refused");
}

// A HUD file: "lupe" lists one model whose joint tree has `flags` on its child; the damage
// counter's scene lists the same model.
Dat hud(uint32_t flags) {
  Joints j = joints(flags);
  Dat& d = j.d;
  const uint32_t model = d.alloc(0x10), list = d.alloc(8), scene = d.alloc(0x10);
  d.ptr(model, j.root);
  d.ptr(list, model);
  d.ptr(scene, list);
  d.roots.push_back({"lupe", list});
  d.roots.push_back({"ScInfDmg_scene_data", scene});
  return d;
}

void test_hud() {
  std::string why;
  auto ok = [&](const Dat& d) { return on(d, [&](const Archive& a) { return hud_models_plain(a, &why); }); };
  check(ok(hud(0)), "a HUD file of plain joints passes", why);
  check(!ok(hud(0x20)), "a HUD model with a particle joint is refused");
  check(!ok(hud(0x4000)), "a HUD model with a spline joint is refused");
}

// An effect file: its data table's programs (a command bank) and textures (a texture bank).
Dat effect(uint32_t width, bool unlisted_textures) {
  Tex t = textures(width);
  Dat& d = t.d;
  d.roots.clear();
  const uint32_t bank = d.alloc(12), list = d.alloc(0x40), table = d.alloc(8);
  d.word(bank + 4, 1); d.word(bank + 8, list - bank);
  d.ptr(table, bank);
  d.ptr(table + 4, t.bank);
  if (unlisted_textures) d.unlist(table + 4);
  d.roots.push_back({"effTestDataTable", table});
  return d;
}

void test_effect() {
  std::string why;
  auto valid = [&](const Dat& d) { return on(d, [&](const Archive& a) { return effect_banks_valid(a, &why); }); };
  check(valid(effect(8, false)), "a whole effect file passes", why);
  check(!valid(effect(8, true)), "an effect table naming its textures by an unlisted word is refused");
  check(!valid(effect(1024, false)), "an effect file whose texture runs past the file is refused");
  const std::vector<uint8_t> clean = effect(8, false).bytes(), same = effect(8, false).bytes(), other = effect(4, false).bytes();
  Archive ca(clean), sa(same), oa(other);
  check(ca.parse() && sa.parse() && oa.parse(), "the effect files parse");
  check(effect_textures_same(ca, sa, &why), "an effect file's textures of the disc's shape pass", why);
  check(!effect_textures_same(ca, oa, &why), "an effect file's textures of another size are refused");
}
}  // namespace

int main() {
  test_collision();
  test_textures();
  test_commands();
  test_joints();
  test_costume();
  test_stage();
  test_hud();
  test_effect();
  if (failed) return 1;
  std::puts("mod data safety checks passed");
  return 0;
}
