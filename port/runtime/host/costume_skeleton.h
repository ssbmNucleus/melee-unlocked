// The costume skeleton walk of the cosmetic importer (cosmetic_checks.cpp, namespace skeleton), opened to
// other host code for any pair of joint roots. `size` is a file's data block size; `relocated` the data
// offsets its relocation table lists, sorted (a pointer field holds a joint only when it is listed).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host::cosmetics::skeleton_shared {

// Walks the standard tree `a` (root ja) and the costume tree `b` (root jb) in lockstep, child first
// then next: same shape, (unless any_flags) same non-visual joint flags, and (unless any_rest_pose)
// rest poses within the importer's tolerance (rotation 1e-2, scale and translation 1e-3, relative
// above 1). `joints` counts the joints walked (start it at 0); `error` names the first difference.
bool same_tree(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
               const std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel,
               bool any_rest_pose, uint32_t* joints, std::string* error, bool any_flags = false);
// Which parts of two joints' rest poses (the nine floats at joint +0x14, `standard` and `costume` point
// at them) differ beyond the importer's tolerance: 1 rotation, 2 scale, 4 translation; 0 when none do.
uint32_t rest_pose_difference(const uint8_t* standard, const uint8_t* costume);
// Whether two rest poses put the joint in the same place: scale and translation within the importer's
// tolerance, and the two rotations the same rotation as a matrix (entries within 1e-2) however the
// three angles are written. Model tools rewrite angles on export: -pi as pi, or X traded for Z when Y
// is +-pi/2 (Sunshine Mario, post 16086, joints 30 and 42 of Mario's skeleton).
bool same_rest_placement(const uint8_t* standard, const uint8_t* costume);
// After same_tree matched the two trees: writes a's 36 rest bytes (joint +0x14) into each joint of b.
// Returns how many joints changed.
uint32_t copy_rest_pose(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
                        std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel);
// False (with the reason) when a mesh under `root` is skinned to a joint with no inverse bind matrix:
// the game stops when it draws it, on any machine.
bool envelopes_bound(const std::vector<uint8_t>& bytes, uint32_t size, const std::vector<uint32_t>& relocated,
                     uint32_t root, std::string* error);
// After same_tree matched the two trees, what else the joint loader reads (jobj.c HSD_JObjLoadJoint):
// `class_names`, each costume joint's class name (+0x00) is the standard joint's (both no pointer and
// the same bytes, or both the same text; a name the game has registered builds the joint as that class);
// `constraints`, no costume joint carries a constraint (robj, +0x3C) the standard one does not.
bool same_joint_build(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
                      const std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel,
                      bool class_names, bool constraints, std::string* error);

}  // namespace host::cosmetics::skeleton_shared
