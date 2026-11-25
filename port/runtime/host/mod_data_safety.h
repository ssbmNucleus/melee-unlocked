// Structure checks for mod data the archive check does not cover: a stage's collision, a particle
// bank and its texture bank, joint trees with particle or spline joints, a costume's joint and mesh
// counts. Each mirrors what the game's loader reads (sourceport/patches/melee-native.patch: mplib.c
// mpCollDataCheck, particle.c ps_banks_check, jobj.c, ftparts.c) so a file the game would refuse or
// cut short is refused here first, with a reason a player can read. Every retail file passes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "disc_costume_metadata.h"

namespace host::cosmetics::data_safety {
using disc_metadata::Archive;

// Bytes of a GX texture: whole tiles of the format's tile size. 0 for a format the game has no use for.
inline uint64_t gx_image_bytes(uint32_t format, uint32_t width, uint32_t height) {
  uint32_t bits, tw, th;
  switch (format) {
    case 0: case 8: case 14: bits = 4; tw = 8; th = 8; break;   // I4, C4, CMPR
    case 1: case 2: case 9: bits = 8; tw = 8; th = 4; break;    // I8, IA4, C8
    case 3: case 4: case 5: case 10: bits = 16; tw = 4; th = 4; break;   // IA8, RGB565, RGB5A3, C14X2
    case 6: bits = 32; tw = 4; th = 4; break;                   // RGBA8
    default: return 0;
  }
  const uint64_t w = (uint64_t(width) + tw - 1) / tw * tw, h = (uint64_t(height) + th - 1) / th * th;
  return w * h * bits / 8;
}
// Bytes of the palette a colour-index format reads.
inline uint64_t gx_palette_bytes(uint32_t format) {
  return format == 8 ? 16 * 2 : format == 9 ? 256 * 2 : format == 10 ? 16384 * 2 : 0;
}

// A texture group's table: textures [0, num), then its palettes. `end` gets the entries the loader
// relocates (particle.c psInitDataBankLocate).
inline uint64_t texture_group_entries(const Archive& a, uint32_t group) {
  const uint32_t num = a.word(group), format = a.word(group + 4);
  const uint32_t palettes = a.word(group + 0x14);
  const uint32_t palnum = palettes >> 16, palflag = palettes & 0xFFFF;
  if (format != 8 && format - 9 > 1) return num;
  return (palflag & 1) ? uint64_t(num) + 1 : palnum ? uint64_t(num) + palnum : uint64_t(num) * 2;
}

// A particle texture bank at `bank` in the data block: a group count, group offsets from the bank,
// each group a 0x18-byte head and a table of texture and palette offsets from the bank. Every
// offset and every texture's bytes stay inside the data block.
inline bool texture_bank_valid(const Archive& a, uint32_t bank, std::string* why) {
  if (!a.bounds(bank, 4)) { *why = "its texture bank is cut short"; return false; }
  const uint64_t groups = a.word(bank);
  if (!a.bounds(bank, (1 + groups) * 4)) { *why = "its texture group count runs past the file"; return false; }
  for (uint64_t i = 1; i <= groups; ++i) {
    const uint32_t off = a.word(bank + i * 4);
    if (!off) continue;
    const uint64_t group = uint64_t(bank) + off;
    if (!a.bounds(group, 0x18)) { *why = "a texture group lies outside the file"; return false; }
    const uint32_t num = a.word(uint32_t(group)), format = a.word(uint32_t(group) + 4);
    const uint32_t width = a.word(uint32_t(group) + 0xC), height = a.word(uint32_t(group) + 0x10);
    if ((a.word(uint32_t(group) + 0x14) >> 16) > 256) { *why = "a texture group has more than 256 palettes"; return false; }
    const uint64_t end = texture_group_entries(a, uint32_t(group));
    if (!a.bounds(group + 0x18, end * 4)) { *why = "a texture table runs past the file"; return false; }
    const uint64_t image = gx_image_bytes(format, width, height), palette = gx_palette_bytes(format);
    for (uint64_t t = 0; t < end; ++t) {
      const uint32_t entry = a.word(uint32_t(group + 0x18 + t * 4));
      if (!entry) continue;
      if (!a.bounds(uint64_t(bank) + entry, t < num ? image : palette)) {
        *why = "a texture lies outside the file"; return false;
      }
    }
    (void)num;
  }
  return true;
}

// A command bank (map_ptcl, an effect file's programs): version 0 with a count and offsets from word 2,
// or 0x40-0x43 with ids below the table in word 1 and a count and offsets from word 3. Every
// command list (0x40-byte head) lies in the file.
inline bool command_bank_valid(const Archive& a, uint32_t bank, std::string* why) {
  if (!a.bounds(bank, 12)) { *why = "its command bank is cut short"; return false; }
  const uint32_t version = a.word(bank) >> 16;
  uint64_t first, count;
  if (version == 0) { first = 2; count = a.word(bank + 4); }
  else if (version >= 0x40 && version < 0x44) { first = 3; count = a.word(bank + 8); }
  else { *why = "its command bank has an unknown version"; return false; }
  if (!a.bounds(bank, (first + count) * 4)) { *why = "its command list count runs past the file"; return false; }
  for (uint64_t i = 0; i < count; ++i) {
    const uint32_t entry = a.word(uint32_t(bank + (first + i) * 4));
    if (entry && !a.bounds(uint64_t(bank) + entry, 0x40)) { *why = "a command list lies outside the file"; return false; }
  }
  return true;
}

// Two texture banks of the same shape: the same groups, each with the same head (count, format,
// size, palettes) and the same textures present. Only the pixels may differ, so the game draws the
// same particles from either; `b` must hold together on its own as well.
inline bool texture_banks_same_shape(const Archive& a, uint32_t ab, const Archive& b, uint32_t bb, std::string* why) {
  if (!texture_bank_valid(b, bb, why)) return false;
  std::string unused;
  if (!texture_bank_valid(a, ab, &unused)) { *why = "the original texture bank cannot be read"; return false; }
  const uint32_t groups = a.word(ab);
  if (b.word(bb) != groups) { *why = "its texture bank has a different number of groups"; return false; }
  for (uint32_t i = 1; i <= groups; ++i) {
    const uint32_t ga = a.word(ab + i * 4), gb = b.word(bb + i * 4);
    if (!ga != !gb) { *why = "texture group " + std::to_string(i - 1) + " is missing or added"; return false; }
    if (!ga) continue;
    const uint32_t pa = ab + ga, pb = bb + gb;
    for (uint32_t k = 0; k < 0x18; k += 4)
      if (a.word(pa + k) != b.word(pb + k)) {
        *why = "texture group " + std::to_string(i - 1) + " has a different format, size or count"; return false;
      }
    const uint64_t end = texture_group_entries(a, pa);
    for (uint64_t t = 0; t < end; ++t)
      if (!a.word(uint32_t(pa + 0x18 + t * 4)) != !b.word(uint32_t(pb + 0x18 + t * 4))) {
        *why = "texture group " + std::to_string(i - 1) + " has a texture missing or added"; return false;
      }
  }
  return true;
}

// A stage's collision (coll_data), as mplib.c mpCollDataCheck reads it: counts within the game's
// arrays (2048 vertices, 1536 lines, 1 to 256 joints), every line group and joint range inside the
// lines, every joint's vertices inside the vertices, every line's vertices and neighbours in range,
// and the three arrays inside the file.
inline bool collision_valid(const Archive& a, uint32_t coll, std::string* why) {
  if (!a.bounds(coll, 0x30)) { *why = "its collision header is cut short"; return false; }
  const int32_t vc = int32_t(a.word(coll + 4)), lc = int32_t(a.word(coll + 12)), jc = int32_t(a.word(coll + 0x28));
  if (vc < 0 || vc > 2048) { *why = "its collision vertex count is out of range"; return false; }
  if (lc < 0 || lc > 1536) { *why = "its collision line count is out of range"; return false; }
  if (jc < 1 || jc > 256) { *why = "its collision joint count is out of range"; return false; }
  uint32_t verts = 0, lines = 0, joints = 0;
  if ((vc && (!a.pointer(coll, &verts) || !a.bounds(verts, uint64_t(vc) * 8))) ||
      (lc && (!a.pointer(coll + 8, &lines) || !a.bounds(lines, uint64_t(lc) * 16))) ||
      !a.pointer(coll + 0x24, &joints) || !a.bounds(joints, uint64_t(jc) * 0x28)) {
    *why = "a collision array runs past the file"; return false;
  }
  auto s16 = [&](uint32_t at) { return int16_t(uint16_t(a.word(at & ~3u) >> ((at & 2) ? 0 : 16))); };
  auto range_ok = [&](uint32_t at) {
    const int start = s16(at), count = s16(at + 2);
    return start >= 0 && count >= 0 && start + count <= lc;
  };
  for (uint32_t g = 0; g < 5; ++g)
    if (!range_ok(coll + 0x10 + g * 4)) { *why = "a collision line group is out of range"; return false; }
  for (int32_t j = 0; j < jc; ++j) {
    const uint32_t at = joints + uint32_t(j) * 0x28;
    for (uint32_t g = 0; g < 5; ++g)
      if (!range_ok(at + g * 4)) { *why = "a collision joint's line group is out of range"; return false; }
    const int start = s16(at + 0x24), count = s16(at + 0x26);
    if (start < 0 || count < 0 || start + count > vc) { *why = "a collision joint's vertices are out of range"; return false; }
  }
  for (int32_t i = 0; i < lc; ++i) {
    const uint32_t at = lines + uint32_t(i) * 16;
    const uint32_t ends = a.word(at);
    if (int32_t(ends >> 16) >= vc || int32_t(ends & 0xFFFF) >= vc) { *why = "a collision line names a vertex out of range"; return false; }
    for (uint32_t k = 0; k < 4; ++k) {
      const int link = s16(at + 4 + k * 2);
      if (link != -1 && (link < 0 || link >= lc)) { *why = "a collision line names a neighbour out of range"; return false; }
    }
  }
  return true;
}

// Every joint reachable from `root` (child, then next, through listed pointers only), at most 4096,
// none twice; an instance joint's child is not followed, as the game's loader does not (jobj.c
// JObjLoad). A particle joint (0x20) with a particle list is refused: the list's links are not
// relocated and the game walks and marks it (jobj.c); no retail costume, stage model or HUD model has
// one. A spline joint (0x4000) is refused unless `splines` (some retail stages have them) and its
// spline is a listed pointer. `meshes` counts the meshes of the first `mesh_joints` joints in that
// order (0: of every joint), the joints a fighter's parts are made from (ftparts.c).
inline bool joint_tree_plain(const Archive& a, uint32_t root, bool splines, std::string* why,
                             uint32_t* joints = nullptr, uint32_t* meshes = nullptr, uint32_t mesh_joints = 0) {
  std::vector<uint32_t> stack{root};
  std::set<uint32_t> seen;
  uint32_t count = 0, dobjs = 0;
  while (!stack.empty()) {
    const uint32_t x = stack.back(); stack.pop_back();
    if (!a.bounds(x, 0x40)) { *why = "a joint lies outside the file"; return false; }
    if (!seen.insert(x).second || ++count > 4096) { *why = "its joint tree loops"; return false; }
    const uint32_t flags = a.word(x + 4);
    uint32_t target = 0;
    if (flags & 0x4000) {   // the game reads the slot as a spline first (jobj.c JObjLoad)
      if (!splines || !a.pointer(x + 0x10, &target)) { *why = "it has a spline joint"; return false; }
    } else if (flags & 0x20) {
      if (a.word(x + 0x10)) { *why = "it has a particle joint"; return false; }
    } else if ((!mesh_joints || count <= mesh_joints) && a.pointer(x + 0x10, &target)) {
      std::set<uint32_t> listed;
      for (uint32_t d = target; listed.insert(d).second && a.bounds(d, 0x10) && listed.size() <= 4096;) {
        ++dobjs;
        if (!a.pointer(d + 4, &d)) break;
      }
    }
    if (a.pointer(x + 0x0C, &target)) stack.push_back(target);
    if (!(flags & 0x1000) && a.pointer(x + 0x08, &target)) stack.push_back(target);
  }
  if (joints) *joints = count;
  if (meshes) *meshes = dobjs;
  return true;
}

// A fighter's costume (the "_Share_joint" tree), by what stops the game or makes it read past the
// file, and nothing else (owner, 2026-11-24: any skin the game can load plays offline, odd models
// included; online is decided apart, costume_served_form.h):
// - plain joints (above): in the file, no loop, no particle joint with a particle list; a spline
//   joint only with its spline listed, as on a stage;
// - no fewer joints than the standard costume of its slot: the fighter's parts table is built for
//   that count and the game stops on a skeleton that runs out first (ftparts.c ftParts_SetupParts,
//   "fighter parts num not match"). More is fine: the joints past it get no part (the game's clamp,
//   ftparts.c under MU_NATIVE), so they follow their parent and are drawn;
// - at most 124 meshes on the joints that get a part: the fighter's mesh list holds 124 and the game
//   stops past that ("fighter parts model dobj num over"; the 128-on-one-joint stop lies beyond it).
// `standard` 0: no standard file to compare with; every joint's meshes are then counted.
constexpr uint32_t kCostumeMeshes = 124;
inline bool costume_structure_valid(const Archive& a, uint32_t standard, std::string* why) {
  uint32_t root, joints = 0, meshes = 0;
  if (!a.root_ending("_Share_joint", &root)) { *why = "it has no skeleton"; return false; }
  if (!joint_tree_plain(a, root, true, why, &joints, &meshes, standard)) return false;
  if (meshes > kCostumeMeshes) {
    *why = "it has " + std::to_string(meshes) + " meshes; a fighter has at most " + std::to_string(kCostumeMeshes);
    return false;
  }
  if (standard && joints < standard) {
    *why = "its skeleton has " + std::to_string(joints) + " joints, fewer than the fighter's " + std::to_string(standard) +
           " (the game stops on that)";
    return false;
  }
  return true;
}
// The joint count of a standard costume (0 when it cannot be read).
inline uint32_t costume_joint_count(const Archive& a) {
  uint32_t root, joints = 0;
  std::string unused;
  if (!a.root_ending("_Share_joint", &root) || !joint_tree_plain(a, root, true, &unused, &joints)) return 0;
  return joints;
}

// A stage file's data the game reads when the stage loads: its collision (coll_data), its particle
// programs and textures (map_ptcl, map_texg) and the joint trees of its models (map_head: a model
// list at +8 with its count at +0xC, 0x34 bytes a model, the joint tree first). Each part checked
// only when the file has it.
inline bool stage_structure_valid(const Archive& a, std::string* why) {
  uint32_t at;
  if (a.root("coll_data", &at) && !collision_valid(a, at, why)) return false;
  if (a.root("map_ptcl", &at) && !command_bank_valid(a, at, why)) return false;
  if (a.root("map_texg", &at) && !texture_bank_valid(a, at, why)) return false;
  uint32_t models;
  if (a.root("map_head", &at) && a.bounds(at, 0x10) && a.pointer(at + 8, &models)) {
    const uint32_t count = a.word(at + 0x0C);
    if (count > 1024 || !a.bounds(models, uint64_t(count) * 0x34)) { *why = "its model list runs past the file"; return false; }
    for (uint32_t m = 0; m < count; ++m) {
      uint32_t joint;
      if (a.pointer(models + m * 0x34, &joint) && !joint_tree_plain(a, joint, true, why)) return false;
    }
  }
  return true;
}

// A HUD file (IfAll): the model lists the game loads from it, each a list of model slots ended by
// one that is no pointer, a model's joint tree first (sc/types.h DynamicModelDesc). The damage
// counter's scene ("ScInfDmg_scene_data") holds its model list first. Every joint tree plain.
inline bool hud_models_plain(const Archive& a, std::string* why) {
  static const char* const kLists[] = {"DmgMrk_scene_models", "DmgNum_scene_models", "ScInfCnt_scene_models",
                                       "ScInfPnm_scene_models", "ScInfStc_scene_models", "ScInfTim_scene_models",
                                       "Stc_rarwmdls", "Stc_scemdls", "lupe", "tdsce"};
  auto list_plain = [&](uint32_t list) {
    uint32_t model, joint;
    for (uint32_t slot = 0; slot < 256 && a.pointer(list + slot * 4, &model); ++slot)
      if (a.bounds(model, 0x10) && a.pointer(model, &joint) && !joint_tree_plain(a, joint, false, why)) return false;
    return true;
  };
  uint32_t at;
  for (const char* name : kLists)
    if (a.root(name, &at) && !list_plain(at)) return false;
  uint32_t list;
  if (a.root("ScInfDmg_scene_data", &at) && a.pointer(at, &list) && !list_plain(list)) return false;
  return true;
}

// An effect file's particle programs and texture bank: words 0 and 1 of its "...DataTable" root
// (efasync.c), each either 0 or a listed pointer to a bank that holds together.
inline bool effect_banks_valid(const Archive& a, std::string* why) {
  uint32_t table, bank;
  if (!a.root_ending("DataTable", &table) || !a.bounds(table, 8)) { *why = "it has no effect table"; return false; }
  for (uint32_t word : {0u, 4u}) {
    if (!a.word(table + word)) continue;
    if (!a.pointer(table + word, &bank)) { *why = "its effect table points outside its tables"; return false; }
    if (!(word ? texture_bank_valid(a, bank, why) : command_bank_valid(a, bank, why))) return false;
  }
  return true;
}

// An effect file's texture bank (word 1 of its "...DataTable" root, efasync.c) against the disc's
// file: present in both or neither, and the same shape (texture_banks_same_shape).
inline bool effect_textures_same(const Archive& clean, const Archive& pack, std::string* why) {
  uint32_t ta, tb;
  if (!clean.root_ending("DataTable", &ta) || !pack.root_ending("DataTable", &tb) || !clean.bounds(ta, 8) ||
      !pack.bounds(tb, 8)) {
    *why = "it has no effect table"; return false;
  }
  uint32_t ba = 0, bb = 0;
  const bool has_a = clean.pointer(ta + 4, &ba), has_b = pack.pointer(tb + 4, &bb);
  if (has_a != has_b) { *why = "its texture bank is missing or added"; return false; }
  return !has_a || texture_banks_same_shape(clean, ba, pack, bb, why);
}
}  // namespace host::cosmetics::data_safety
