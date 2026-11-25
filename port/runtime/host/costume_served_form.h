// The served form of a costume skin for one costume slot: what an opponent's skin becomes before this
// machine would hand it to the game in place of the retail costume file of the slot they picked.
//
// The simulation reads a fighter's bones from its costume file, so the served form keeps the skin's
// meshes, materials and textures and makes its skeleton the retail slot's, bit for bit. Each machine
// builds it alone from its own retail disc; nothing about it is agreed over the network.
//
// What the game reads from a costume's skeleton (sourceport/extern/melee, checked 2026-11-25): the
// fighter's bones ARE the costume's joint tree (Fighter_UnkUpdateCostumeJoint_800686E4 loads
// fp->x108_costume_joint; the shared PlXx.dat only names bones by index for hurtboxes, hitboxes and the
// ECB). Per joint the simulation reads:
//   - the tree's shape (which bone is which index): compared, a difference refuses (step 5);
//   - the non-visual flags (billboards, quaternions, IK joint types, user or independent matrices),
//     which change how the bone matrix is made: compared, a difference refuses (step 5);
//   - a constraint (robj, +0x3C), which moves the bone: refused (step 5);
//   - the class name (+0x00): HSD_JObjLoadJoint (jobj.c) builds a joint whose description names a class
//     the game has registered as that class (hsdSearchClassInfo, hsdNew), and reads a value the
//     relocation table does not list as an address all the same; retail costume joints name none:
//     compared, a difference refuses (step 5; policy);
//   - the rest rotation, scale and translation (+0x14, nine floats). Read at load, by
//     Fighter_UnkUpdateVecFromBones_8006876C, and again at EVERY action change: ftAnim_8006EBE8 ->
//     ftAnim_8006FA58 -> lb_8000B4FC copies the costume's rest values into every part, then the action's
//     figatree overwrites only the channels it has tracks for. Channels an action does not animate keep
//     the costume's rest value for the whole action (Mario's joint 30 has no rotation track in 193 of his
//     195 actions; joint 42 no X or Z rotation track in 20). Replaced by retail's (step 7), never kept.
// Not read by the simulation: meshes, materials, textures, the inverse bind matrix (+0x38, drawing
// only: pobj.c, ftparts.c ftPartsSetupEnvelopeMtx, ftdrawcommon.c) and the visual flags.
//
// costume_served_form(retail, skin, target):
//   1. `retail` must be the retail file of `target` (identity by its own root symbols).
//   2. The skin passes the importer's structure check (dat_check.h, cosmetic rules) and is a costume
//      of the target's fighter: exactly one Ply<Fighter>5K<colour>_Share_joint root. Its own colour
//      must be a retail slot unless policy.allow_extra_colour_source (m-ex extra colours).
//   3. Its root symbols are renamed to the target slot's (Ply<Fighter>5K<colour>_..., and
//      Jigglypuff's PlyPurin<colour>Hat_... roots), and the result must identify as exactly the
//      target slot (the importer's identity check).
//   4. It has every root of the retail file and no other joint root (a symbol ending in _joint). Other
//      extra roots (m-ex's "mexCostume") are data the game never asks a costume for and stay unread.
//   5. Every skeleton root (a symbol ending in _joint that is not a matanim or animjoint, Jigglypuff's
//      hats included) walks in lockstep with the retail tree of the same name: same shape, same
//      non-visual joint flags (policy: or any), and a rest pose within the importer's tolerance
//      (policy: or any size, which step 7 replaces; an opponent's skin is served by that policy,
//      peer_skin_policy below); no joint carries a constraint (robj, joint +0x3C; policy: not looked
//      at); and (policy) every joint's class name (+0x00) is the retail joint's.
//   6. The fighter's main tree has at most 124
//      display objects in all and fewer than 128 on any one joint (ftparts.c asserts past either);
//      no mesh is skinned to a joint without a bind matrix (the importer's draw-safety check); and
//      the importer's structure rule for an offline costume holds (mod_data_safety.h
//      costume_structure_valid: what stops the game or reads past the file), so a skin the importer
//      refuses is never served.
//   7. The retail rest pose (joint +0x14, 36 bytes) is copied into every joint of every skeleton
//      root, then checked to be bit-equal to retail. The reason counts the joints that were past the
//      importer's tolerance, and how many of them were placed differently (the skin is then drawn on
//      the retail bones there) rather than the same rotation written another way.
// Any failure: ok false and the reason; the caller serves the retail costume instead.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host::skins {

struct ServedFormPolicy {
  // false (the client's import rule, the default; offline it decides between the retail bones and the
  // skin as installed): a rest pose that differs from retail by more than the importer's tolerance
  // refuses the skin. true: any rest pose is replaced (Nucleus's rule; online, own and peer skins).
  bool snap_any_rest_pose = false;
  // A skin made for a colour the retail fighter does not have (m-ex extra colours). Off by default.
  bool allow_extra_colour_source = false;
  // Step 5: a joint whose non-visual flags differ from retail's refuses the skin. Nucleus does not look.
  bool compare_flags = true;
  // Step 4 and 5: a joint root the retail file does not have, and a joint constraint (robj), refuse
  // the skin. Neither stops the game (the game never asks for the root; retail and the Vault have no
  // constraints). Nucleus does not look.
  bool refuse_extras = true;
  // Step 5: a joint whose class name (+0x00) is not the retail joint's refuses the skin. Off by default
  // (offline unchanged); on for every online form (peer_skin_policy).
  bool compare_class_names = false;
};

// An opponent's skin on this machine, and what this machine announces of its own (the opponent's
// machine builds the same form from the same file, so both play one fighter): strict wherever the
// simulation could differ, and a rest pose never refuses (2026-11-25). Every joint of every skeleton
// root gets the retail rest pose of the slot played (step 7, proven bit-equal), so the bones the game
// simulates with are retail's whatever the skin's rest pose was; refusing a rest pose protected nothing
// the replacement does not. It refused Sunshine Mario (post 16086, "NETPLAY SAFE"): two of Mario's bones
// were the same rotation written as other angles (-pi as pi; X traded for Z at Y = -pi/2), a model
// tool's export. Still refused, so the opponent sees the retail costume: another joint count or tree
// shape, a missing or unknown skeleton root, a non-visual joint flag that differs, a joint constraint,
// a joint class name that differs, a color the retail fighter does not have, and anything the game
// could not load or draw. Where a skin's bone really sits elsewhere at rest, its mesh is drawn on the
// retail bone there (the reason says how many).
constexpr bool kStrictPeerRestPose = false;    // true: the rule before 2026-11-25 (importer's tolerance refuses)
inline ServedFormPolicy peer_skin_policy() {
  ServedFormPolicy p;
  p.snap_any_rest_pose = !kStrictPeerRestPose;
  p.compare_class_names = true;
  return p;
}

// The player's own skin online, on their own screen: the peer rule itself (2026-11-25). Online a skin is
// either shown on both machines on a skeleton bit-equal to retail, or on neither: this machine never
// simulates with a skin an opponent's machine would refuse, and what it refuses its own player sees as
// the retail costume too. (Until 2026-11-25 this was the desktop app's rule, which let a non-visual flag,
// a constraint, an extra joint root and an m-ex extra color through on the wearer's screen only.) The
// disc-slot path (a mod disc, a pack, an L / R pick) plays by the same comparisons: cosmetic_mods.h
// costume_online_form. Offline is not this: costume_offline_form.
inline ServedFormPolicy own_skin_policy() { return peer_skin_policy(); }

struct ServedForm {
  bool ok = false;
  std::vector<uint8_t> bytes;   // the file to serve (ok only)
  std::string reason;           // why not; when ok, a one-line summary
  std::string source_slot;      // the slot the skin was made for ("PlFxGr.dat"; empty when unknown)
  uint32_t joints = 0;          // joints compared, over every skeleton root
  uint32_t snapped = 0;         // joints whose rest pose was replaced
  uint32_t skeleton_roots = 0;
  uint32_t past_tolerance = 0;  // of those, past the importer's tolerance (snap_any_rest_pose only)
  uint32_t placed_elsewhere = 0;   // of those, not the same rotation written another way: drawn differently
};

constexpr uint32_t kMaxFighterDObjs = 124;      // fighter->dobj_list: 0x1F0 / 4 entries (fighter.c, ftparts.c)
constexpr uint32_t kMaxDObjsPerJoint = 127;     // ftparts.c: "fighter dobj num over" at 128 on one joint

// `target_slot`: the retail costume file name, "PlFxGr.dat".
ServedForm costume_served_form(const std::vector<uint8_t>& retail_slot_bytes, const std::vector<uint8_t>& skin_bytes,
                               const std::string& target_slot, const ServedFormPolicy& policy = {});

// Offline, a skin in the slot it was made for (owner, 2026-11-24: skins play offline as the desktop app
// installs them, odd models included): the strict form above when the skin passes it (its rest pose
// within the importer's tolerance; the bones are then retail's, as before), else the skin as installed,
// its own bones and all, when the game can load it: the archive check, the slot's identity, no mesh
// skinned to a bone without a bind matrix, and mod_data_safety.h costume_structure_valid against the
// retail slot's joint count (no fewer joints, at most 124 listed meshes, no particle list). Never
// used for an online match, a replay or anything another machine must match.
ServedForm costume_offline_form(const std::vector<uint8_t>& retail_slot_bytes, const std::vector<uint8_t>& skin_bytes,
                                const std::string& target_slot);

// The form a player's own installed skin plays in on their own machine (the costume slots'
// build_locked): online costume_served_form by own_skin_policy (the peer rule; not ok: the retail
// costume, on both machines), offline costume_offline_form.
ServedForm wearer_form(const std::vector<uint8_t>& retail_slot_bytes, const std::vector<uint8_t>& skin_bytes,
                       const std::string& target_slot, bool online);

// A skeleton root by Nucleus's rule: ends in "_joint", not a material or animation joint.
bool skeleton_root_symbol(const std::string& symbol);

// For tests and the corpus tool: whether every skeleton root of `served` is the retail one in all the
// simulation reads: the trees have the same shape, every joint the same rest pose bit for bit, the same
// non-visual flags, the same class name and no constraint the retail joint lacks.
bool skeleton_identical(const std::vector<uint8_t>& retail, const std::vector<uint8_t>& served, std::string* why);

}  // namespace host::skins
