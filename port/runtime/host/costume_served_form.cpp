// The served form of an opponent's costume skin (costume_served_form.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "costume_served_form.h"
#include "cosmetic_mods.h"
#include "costume_skeleton.h"
#include "dat_check.h"
#include "mod_data_safety.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace host::skins {
namespace {

using host::dat::be32;

void put_be32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }
bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A whole archive by the importer's rules, with its root table and sorted relocation list.
struct Dat {
  uint32_t data_size = 0;
  std::vector<uint32_t> relocated;
  std::vector<std::pair<std::string, uint32_t>> roots;   // symbol, data offset
};

bool parse(const std::vector<uint8_t>& b, Dat* d, std::string* error) {
  std::vector<std::string> names;
  *error = host::dat::structure_error(b.data(), b.size(), host::dat::Rules::Cosmetic, &names);
  if (!error->empty()) return false;
  // structure_error checked every table against the file; these reads stay inside it.
  d->data_size = be32(b.data() + 4);
  const uint64_t relocations = be32(b.data() + 8), roots = be32(b.data() + 12);
  const uint64_t table = 0x20ull + d->data_size + relocations * 4;
  d->relocated.resize((size_t)relocations);
  for (uint64_t i = 0; i < relocations; ++i) d->relocated[(size_t)i] = be32(b.data() + 0x20 + d->data_size + i * 4);
  std::sort(d->relocated.begin(), d->relocated.end());
  d->roots.clear();
  for (uint64_t i = 0; i < roots && i < names.size(); ++i) d->roots.emplace_back(names[(size_t)i], be32(b.data() + table + i * 8));
  return true;
}

bool is_pointer(const Dat& d, uint64_t slot) {
  return slot <= UINT32_MAX && std::binary_search(d.relocated.begin(), d.relocated.end(), (uint32_t)slot);
}

// The joints of one tree, depth first (child, then next), as the skeleton walk numbers them.
// Stops past 1024 (a cycle or a broken file): the caller has already walked the tree with same_tree.
std::vector<uint32_t> joints_of(const std::vector<uint8_t>& b, const Dat& d, uint32_t root) {
  std::vector<uint32_t> order, stack{root};
  while (!stack.empty() && order.size() <= 1024) {
    const uint32_t x = stack.back(); stack.pop_back();
    if (x + 0x40ull > d.data_size) continue;
    order.push_back(x);
    const uint8_t* p = &b[0x20ull + x];
    if (is_pointer(d, x + 0x0Cull)) stack.push_back(be32(p + 0x0C));
    if (is_pointer(d, x + 0x08ull)) stack.push_back(be32(p + 0x08));
  }
  return order;
}

// Renames root symbols by prefix. The data block, relocations and references keep their bytes; the
// string table is written anew after the root and reference records (as the m-ex alias does).
bool rename_roots(const std::vector<uint8_t>& bytes, const std::vector<std::pair<std::string, std::string>>& prefixes,
                  std::vector<uint8_t>* out, std::string* error) {
  const uint64_t dsize = be32(bytes.data() + 4), relocations = be32(bytes.data() + 8), roots = be32(bytes.data() + 12),
                 references = be32(bytes.data() + 16);
  const uint64_t table = 0x20ull + dsize + relocations * 4, names = table + (roots + references) * 8;
  if (names > bytes.size()) { *error = "DAT tables point outside the file."; return false; }
  std::vector<uint8_t> rewritten(bytes.begin(), bytes.begin() + (size_t)names);
  for (uint64_t i = 0; i < roots + references; ++i) {
    const uint64_t at = names + be32(bytes.data() + table + i * 8 + 4);
    if (at >= bytes.size()) { *error = "DAT symbol points outside the file."; return false; }
    const char* text = (const char*)bytes.data() + at;
    const size_t length = strnlen(text, bytes.size() - (size_t)at);
    if (!length || length > 255 || length == bytes.size() - at) { *error = "DAT symbol is not bounded."; return false; }
    std::string symbol(text, length);
    if (i < roots)
      for (const auto& rule : prefixes)
        if (starts_with(symbol, rule.first)) { symbol = rule.second + symbol.substr(rule.first.size()); break; }
    if (symbol.size() > 255) { *error = "A renamed symbol is too long."; return false; }
    put_be32(rewritten.data() + table + i * 8 + 4, (uint32_t)(rewritten.size() - names));
    rewritten.insert(rewritten.end(), symbol.begin(), symbol.end());
    rewritten.push_back(0);
  }
  put_be32(rewritten.data(), (uint32_t)rewritten.size());
  *out = std::move(rewritten);
  return true;
}

// "PlyFox5KGr_Share_joint" -> fighter "Fox", colour "Gr" ("" for the default colour).
bool share_symbol_parts(const std::string& symbol, std::string* fighter, std::string* colour) {
  static const std::string head = "Ply", tail = "_Share_joint";
  if (!starts_with(symbol, head) || !ends_with(symbol, tail)) return false;
  const std::string middle = symbol.substr(head.size(), symbol.size() - head.size() - tail.size());
  const size_t k = middle.rfind("5K");
  if (k == std::string::npos || k == 0) return false;
  const std::string c = middle.substr(k + 2);
  if (!(c.empty() || (c.size() == 2 && std::isupper((unsigned char)c[0]) && std::islower((unsigned char)c[1])))) return false;
  *fighter = middle.substr(0, k);
  *colour = c;
  return true;
}

ServedForm refuse(ServedForm r, const std::string& why) {
  r.ok = false; r.bytes.clear(); r.reason = why;
  return r;
}

// skeleton_identical, with the comparisons beyond the shape and the rest pose each by a policy switch
// (step 7: what the policy compared in step 5).
bool skeleton_same(const std::vector<uint8_t>& retail_bytes, const std::vector<uint8_t>& served, std::string* why,
                   bool flags, bool class_names, bool constraints);

}  // namespace

bool skeleton_root_symbol(const std::string& symbol) {
  return ends_with(symbol, "_joint") && symbol.find("matanim") == std::string::npos &&
         symbol.find("animjoint") == std::string::npos;
}

ServedForm costume_served_form(const std::vector<uint8_t>& retail_bytes, const std::vector<uint8_t>& skin_bytes,
                               const std::string& target_slot, const ServedFormPolicy& policy) {
  ServedForm result;
  std::string error;
  // 1. The reference is the retail file of the target slot.
  const auto retail_identity = host::cosmetics::testing::inspect_dat(retail_bytes);
  if (!retail_identity.ok || lower(retail_identity.target_path) != lower(target_slot))
    return refuse(result, "the reference file is not the retail " + target_slot);
  Dat retail;
  if (!parse(retail_bytes, &retail, &error)) return refuse(result, "the reference file: " + error);
  std::string fighter, target_colour;
  for (const auto& root : retail.roots)
    if (share_symbol_parts(root.first, &fighter, &target_colour)) break;
  if (fighter.empty()) return refuse(result, "the reference file has no skeleton root");

  // 2. The skin: a whole archive, a costume of this fighter.
  Dat skin;
  if (!parse(skin_bytes, &skin, &error)) return refuse(result, "structure: " + error);
  std::vector<std::string> sources;
  for (const auto& root : skin.roots) {
    std::string f, c;
    if (share_symbol_parts(root.first, &f, &c) && f == fighter) sources.push_back(c);
  }
  if (sources.empty()) return refuse(result, "not a " + fighter + " costume (no Ply" + fighter + "5K.._Share_joint root)");
  if (sources.size() > 1) return refuse(result, "names more than one " + fighter + " costume");
  const std::string source_colour = sources[0];
  const auto skin_identity = host::cosmetics::testing::inspect_dat(skin_bytes);
  if (skin_identity.ok) result.source_slot = skin_identity.target_path;
  if (!skin_identity.ok && !policy.allow_extra_colour_source)
    return refuse(result, "no retail slot: made for colour " + (source_colour.empty() ? std::string("Nr") : source_colour) +
                              ", which the retail " + fighter + " does not have (" + skin_identity.error + ")");

  // 3. Renamed to the target slot, and identified as it.
  std::vector<uint8_t> served;
  if (source_colour != target_colour) {
    std::vector<std::pair<std::string, std::string>> prefixes = {
        {"Ply" + fighter + "5K" + source_colour + "_", "Ply" + fighter + "5K" + target_colour + "_"}};
    // Jigglypuff's hats: PlyPurinReHat_TopN_joint (the default colour has none).
    prefixes.push_back({"Ply" + fighter + source_colour + "Hat_", "Ply" + fighter + target_colour + "Hat_"});
    if (!rename_roots(skin_bytes, prefixes, &served, &error)) return refuse(result, "rename: " + error);
  } else {
    served = skin_bytes;
  }
  const auto served_identity = host::cosmetics::testing::inspect_dat(served);
  if (!served_identity.ok) return refuse(result, "identity after renaming: " + served_identity.error);
  if (lower(served_identity.target_path) != lower(target_slot))
    return refuse(result, "identity after renaming: " + served_identity.target_path + ", not " + target_slot);
  Dat form;
  if (!parse(served, &form, &error)) return refuse(result, "structure after renaming: " + error);

  // 4. The same roots as the retail file.
  std::map<std::string, uint32_t> retail_roots, form_roots;
  for (const auto& root : retail.roots) retail_roots.emplace(root.first, root.second);
  for (const auto& root : form.roots)
    if (!form_roots.emplace(root.first, root.second).second) return refuse(result, "root " + root.first + " is named twice");
  for (const auto& root : form_roots) {
    if (retail_roots.count(root.first)) continue;
    // A root that names no joint tree is data the game never asks a costume file for (m-ex exports
    // add "mexCostume"): it stays in the file unread. Any other joint root is refused (policy: kept
    // unread too; the game only asks for the retail file's roots).
    if (!ends_with(root.first, "_joint") || !policy.refuse_extras) continue;
    return refuse(result, std::string(skeleton_root_symbol(root.first) ? "unknown skeleton root " : "unknown joint root ") +
                              root.first + " (the retail file has no such root)");
  }
  for (const auto& root : retail_roots)
    if (!form_roots.count(root.first)) return refuse(result, "missing root " + root.first);

  // 5. Every skeleton root against the retail tree of the same name.
  std::vector<std::pair<uint32_t, uint32_t>> trees;   // (retail root, served root)
  std::string share;
  for (const auto& root : retail.roots) {
    if (!skeleton_root_symbol(root.first)) continue;
    const uint32_t mine = form_roots[root.first];
    uint32_t joints = 0;
    std::string why;
    if (!host::cosmetics::skeleton_shared::same_tree(retail_bytes, retail.data_size, root.second, retail.relocated, served,
                                                     form.data_size, mine, form.relocated, policy.snap_any_rest_pose,
                                                     &joints, &why, !policy.compare_flags))
      return refuse(result, root.first + ": " + why);
    if ((policy.compare_class_names || policy.refuse_extras) &&
        !host::cosmetics::skeleton_shared::same_joint_build(retail_bytes, retail.data_size, root.second, retail.relocated, served,
                                                            form.data_size, mine, form.relocated, policy.compare_class_names,
                                                            policy.refuse_extras, &why))
      return refuse(result, root.first + ": " + why);
    result.joints += joints;
    trees.emplace_back(root.second, mine);
    if (ends_with(root.first, "_Share_joint")) share = root.first;
  }
  result.skeleton_roots = (uint32_t)trees.size();
  if (trees.empty() || share.empty()) return refuse(result, "the retail file has no skeleton");

  // 6. Display object counts, bind matrices.
  for (const auto& tree : trees) {
    std::string why;
    if (!host::cosmetics::skeleton_shared::envelopes_bound(served, form.data_size, form.relocated, tree.second, &why))
      return refuse(result, "draw safety: " + why);
  }
  {
    constexpr uint32_t kNoMeshFlags = 0x20u | 0x4000u;   // particle and spline joints keep other data in the mesh slot
    const auto order = joints_of(served, form, form_roots[share]);
    uint32_t total = 0;
    for (size_t j = 0; j < order.size(); ++j) {
      const uint8_t* p = &served[0x20ull + order[j]];
      if ((be32(p + 4) & kNoMeshFlags) || !is_pointer(form, order[j] + 0x10ull)) continue;
      uint32_t here = 0;
      for (uint32_t d = be32(p + 0x10);;) {
        if (d + 0x10ull > form.data_size) break;
        ++here; ++total;
        if (here > kMaxDObjsPerJoint)
          return refuse(result, "joint " + std::to_string(j) + " has 128 or more display objects; the game stops at 128");
        if (total > kMaxFighterDObjs)
          return refuse(result, "more than " + std::to_string(kMaxFighterDObjs) + " display objects; the game stops past " +
                                    std::to_string(kMaxFighterDObjs));
        if (!is_pointer(form, d + 0x04ull)) break;
        d = be32(&served[0x20ull + d + 0x04]);
      }
    }
  }

  // 6b. The importer's own structure rule for a costume (mod_data_safety.h, the check an offline skin
  // passes before it reaches the game): what would stop the game or have it read past the file. Step 5
  // has already made the skeleton the retail one's shape, so its joint count is the retail slot's.
  {
    host::cosmetics::data_safety::Archive retail_archive(retail_bytes), served_archive(served);
    const uint32_t standard =
        retail_archive.parse() ? host::cosmetics::data_safety::costume_joint_count(retail_archive) : 0;
    std::string why;
    if (!standard) return refuse(result, "the reference file's skeleton cannot be counted");
    if (!served_archive.parse()) return refuse(result, "structure: its archive tables do not hold together");
    if (!host::cosmetics::data_safety::costume_structure_valid(served_archive, standard, &why))
      return refuse(result, "structure: " + why);
  }

  // 7. The retail rest pose, then proof that every skeleton joint has it bit for bit. First, for the
  // reason only: the joints past the importer's tolerance (step 5 let them through under
  // snap_any_rest_pose), and of those the ones placed differently, not the same rotation written as
  // other angles. Neither changes what is served: the simulation gets retail's values either way.
  std::string past;   // "30, 42" (a joint of another skeleton root: "<root> 1")
  for (const auto& tree : trees) {
    const auto a = joints_of(retail_bytes, retail, tree.first), b = joints_of(served, form, tree.second);
    std::string root;
    for (const auto& r : retail.roots)
      if (r.second == tree.first && skeleton_root_symbol(r.first) && !ends_with(r.first, "_Share_joint")) root = r.first + " ";
    for (size_t j = 0; j < a.size() && j < b.size(); ++j) {
      const uint8_t* p = &retail_bytes[0x20ull + a[j] + 0x14];
      const uint8_t* q = &served[0x20ull + b[j] + 0x14];
      if (!host::cosmetics::skeleton_shared::rest_pose_difference(p, q)) continue;
      if (++result.past_tolerance <= 4) past += (past.empty() ? "" : ", ") + root + std::to_string(j);
      if (!host::cosmetics::skeleton_shared::same_rest_placement(p, q)) ++result.placed_elsewhere;
    }
  }
  for (const auto& tree : trees)
    result.snapped += host::cosmetics::skeleton_shared::copy_rest_pose(retail_bytes, retail.data_size, tree.first, retail.relocated,
                                                                      served, form.data_size, tree.second, form.relocated);
  std::string why;
  if (!skeleton_same(retail_bytes, served, &why, policy.compare_flags, policy.compare_class_names, policy.refuse_extras))
    return refuse(result, "after copying the retail rest pose: " + why);
  result.ok = true;
  result.bytes = std::move(served);
  result.reason = std::to_string(result.joints) + " joints in " + std::to_string(result.skeleton_roots) +
                  " skeleton roots match " + target_slot + "; " + std::to_string(result.snapped) + " rest poses replaced";
  if (result.past_tolerance) {
    result.reason += " (" + std::to_string(result.past_tolerance) + " past the importer's tolerance, joint" +
                     (result.past_tolerance > 1 ? "s " : " ") + past + (result.past_tolerance > 4 ? ", ..." : "") + ": ";
    result.reason += result.placed_elsewhere
                         ? std::to_string(result.placed_elsewhere) + " placed elsewhere, drawn on the retail bones)"
                         : "the same placement written as other angles)";
  }
  return result;
}

ServedForm costume_offline_form(const std::vector<uint8_t>& retail_bytes, const std::vector<uint8_t>& skin_bytes,
                                const std::string& target_slot) {
  ServedForm strict = costume_served_form(retail_bytes, skin_bytes, target_slot);
  if (strict.ok) return strict;
  ServedForm result;
  result.source_slot = strict.source_slot;
  std::string error;
  const auto retail_identity = host::cosmetics::testing::inspect_dat(retail_bytes);
  if (!retail_identity.ok || lower(retail_identity.target_path) != lower(target_slot))
    return refuse(result, "the reference file is not the retail " + target_slot);
  Dat skin;
  if (!parse(skin_bytes, &skin, &error)) return refuse(result, "structure: " + error);
  const auto skin_identity = host::cosmetics::testing::inspect_dat(skin_bytes);
  if (!skin_identity.ok) return refuse(result, "identity: " + skin_identity.error);
  result.source_slot = skin_identity.target_path;
  if (lower(skin_identity.target_path) != lower(target_slot))
    return refuse(result, "made for " + skin_identity.target_path + ", not " + target_slot);
  // The game stops drawing a mesh skinned to a bone with no bind matrix (pobj.c), on any skeleton root.
  for (const auto& root : skin.roots) {
    if (!skeleton_root_symbol(root.first)) continue;
    ++result.skeleton_roots;
    std::string why;
    if (!host::cosmetics::skeleton_shared::envelopes_bound(skin_bytes, skin.data_size, skin.relocated, root.second, &why))
      return refuse(result, "draw safety: " + why);
  }
  host::cosmetics::data_safety::Archive retail_archive(retail_bytes), skin_archive(skin_bytes);
  const uint32_t standard = retail_archive.parse() ? host::cosmetics::data_safety::costume_joint_count(retail_archive) : 0;
  if (!standard) return refuse(result, "the reference file's skeleton cannot be counted");
  if (!skin_archive.parse()) return refuse(result, "structure: its archive tables do not hold together");
  std::string why;
  if (!host::cosmetics::data_safety::costume_structure_valid(skin_archive, standard, &why)) return refuse(result, "structure: " + why);
  result.joints = host::cosmetics::data_safety::costume_joint_count(skin_archive);
  result.ok = true;
  result.bytes = skin_bytes;
  result.reason = "as installed, its own bones (" + std::to_string(result.joints) + " joints, the retail costume " +
                  std::to_string(standard) + "; not the retail skeleton: " + strict.reason + ")";
  return result;
}

ServedForm wearer_form(const std::vector<uint8_t>& retail_bytes, const std::vector<uint8_t>& skin_bytes,
                       const std::string& target_slot, bool online) {
  return online ? costume_served_form(retail_bytes, skin_bytes, target_slot, own_skin_policy())
                : costume_offline_form(retail_bytes, skin_bytes, target_slot);
}

bool skeleton_identical(const std::vector<uint8_t>& retail_bytes, const std::vector<uint8_t>& served, std::string* why) {
  return skeleton_same(retail_bytes, served, why, true, true, true);
}

namespace {
bool skeleton_same(const std::vector<uint8_t>& retail_bytes, const std::vector<uint8_t>& served, std::string* why,
                   bool flags, bool class_names, bool constraints) {
  std::string local; if (!why) why = &local;
  Dat a, b;
  if (!parse(retail_bytes, &a, why) || !parse(served, &b, why)) return false;
  std::map<std::string, uint32_t> broots;
  for (const auto& root : b.roots) broots.emplace(root.first, root.second);
  for (const auto& root : a.roots) {
    if (!skeleton_root_symbol(root.first)) continue;
    const auto other = broots.find(root.first);
    if (other == broots.end()) { *why = "no root " + root.first; return false; }
    // The shape, the non-visual flags, the class names and the constraints (the rest pose, bit for bit, below).
    uint32_t walked = 0;
    if (!host::cosmetics::skeleton_shared::same_tree(retail_bytes, a.data_size, root.second, a.relocated, served, b.data_size,
                                                     other->second, b.relocated, true, &walked, why, !flags) ||
        ((class_names || constraints) &&
         !host::cosmetics::skeleton_shared::same_joint_build(retail_bytes, a.data_size, root.second, a.relocated, served,
                                                             b.data_size, other->second, b.relocated, class_names, constraints, why))) {
      *why = root.first + ": " + *why;
      return false;
    }
    struct Pair { uint32_t x, y; bool hx, hy; };
    std::vector<Pair> stack{{root.second, other->second, true, true}};
    uint32_t joint = 0;
    while (!stack.empty()) {
      const Pair top = stack.back(); stack.pop_back();
      if (top.hx != top.hy) { *why = root.first + ": the trees differ in shape at joint " + std::to_string(joint); return false; }
      if (!top.hx) continue;
      if (top.x + 0x40ull > a.data_size || top.y + 0x40ull > b.data_size || ++joint > 1024) {
        *why = root.first + ": joint outside the data block"; return false;
      }
      const uint8_t* p = &retail_bytes[0x20ull + top.x];
      const uint8_t* q = &served[0x20ull + top.y];
      if (std::memcmp(p + 0x14, q + 0x14, 0x24) != 0) {
        *why = root.first + ": joint " + std::to_string(joint - 1) + " rest pose is not retail's"; return false;
      }
      stack.push_back({be32(p + 0x0C), be32(q + 0x0C), is_pointer(a, top.x + 0x0Cull), is_pointer(b, top.y + 0x0Cull)});
      stack.push_back({be32(p + 0x08), be32(q + 0x08), is_pointer(a, top.x + 0x08ull), is_pointer(b, top.y + 0x08ull)});
    }
  }
  return true;
}
}  // namespace

}  // namespace host::skins
