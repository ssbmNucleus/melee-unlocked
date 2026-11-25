// Cosmetic checks of costume files: what the file holds, the skeleton walk and rest pose fix,
// drawable, visual only, pause screens, structure.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

testing::DatInspection inspect_dat_impl(const std::vector<uint8_t>& bytes) {
  testing::DatInspection result;
  // The archive's structure (host/dat_check.h), with the stricter rules of a cosmetic import.
  result.error = host::dat::structure_error(bytes.data(), bytes.size(), host::dat::Rules::Cosmetic, &result.roots);
  if (!result.error.empty()) { result.roots.clear(); return result; }

  struct Match { const FighterFamily* family; const ColorName* color; };
  std::vector<Match> matches;
  for (const auto& family : families) {
    std::string allowed = std::string(" ") + family.colors + " ";
    for (const auto& color : color_names) {
      if (allowed.find(std::string(" ") + color.code + " ") == std::string::npos) continue;
      std::string identity = std::string("Ply") + family.root_name + "5K" +
                             (std::strcmp(color.code, "Nr") ? color.code : "") +
                             "_Share_joint";
      if (std::find(result.roots.begin(), result.roots.end(), identity) != result.roots.end())
        matches.push_back({&family, &color});
    }
  }
  if (matches.size() == 1) {
    result.ok = true;
    result.character = matches[0].family->display_name;
    result.costume = matches[0].color->label;
    result.target_path = std::string("Pl") + matches[0].family->file_code +
                         matches[0].color->code + ".dat";
    return result;
  }
  if (matches.size() > 1) {
    result.error = "DAT roots conflict across multiple existing costume slots."; return result;
  }
  result.error = "DAT roots do not identify a supported existing costume slot.";
  return result;
}

uint32_t gx_image_size(uint32_t width, uint32_t height, uint32_t format,
                       uint32_t mipmap, float max_lod) {
  uint32_t block_width = 4, block_height = 4, block_bytes = 32;
  switch (format) {
    case 0: case 8: case 14: block_width = 8; block_height = 8; break;
    case 1: case 2: case 9: block_width = 8; block_height = 4; break;
    case 6: block_bytes = 64; break;
    case 3: case 4: case 5: case 10: break;
    default: return 0;
  }
  uint32_t levels = mipmap ? (uint32_t)max_lod + 1 : 1, total = 0;
  for (uint32_t level = 0; level < levels; ++level) {
    uint64_t blocks = ((uint64_t)width + block_width - 1) / block_width *
                      (((uint64_t)height + block_height - 1) / block_height);
    if (blocks > (UINT32_MAX - total) / block_bytes) return 0;
    total += (uint32_t)blocks * block_bytes;
    width = std::max(1u, width / 2); height = std::max(1u, height / 2);
  }
  return total;
}

bool parse_visual_layout(const std::vector<uint8_t>& bytes, VisualLayout* out,
                         std::string* error) {
  out->relocations.clear(); out->roots.clear(); out->images.clear(); out->palettes.clear();
  if (bytes.size() < 0x20 || bytes.size() > kMaxAssetBytes || be32(bytes.data()) != bytes.size()) {
    *error = "Visual DAT header size does not match its bounded file length."; return false;
  }
  uint32_t data_size = be32(bytes.data() + 4), relocations = be32(bytes.data() + 8),
           roots = be32(bytes.data() + 12), references = be32(bytes.data() + 16);
  uint64_t relocation_start = 0x20ull + data_size;
  uint64_t root_start = relocation_start + (uint64_t)relocations * 4;
  uint64_t strings = root_start + (uint64_t)(roots + references) * 8;
  if (!roots || roots > 1024 || references > 1024 || relocations > bytes.size() / 4 ||
      strings > bytes.size()) {
    *error = "Visual DAT tables are outside supported bounds."; return false;
  }
  out->data_size = data_size; out->relocation_start = (uint32_t)relocation_start;
  for (uint32_t i = 0; i < relocations; ++i) {
    uint32_t offset = be32(bytes.data() + relocation_start + (uint64_t)i * 4);
    if ((offset & 3) || (uint64_t)offset + 4 > data_size) {
      *error = "Visual DAT relocation is unaligned or outside its data block."; return false;
    }
    uint32_t target = be32(bytes.data() + 0x20ull + offset);
    if (target && target >= data_size) {
      *error = "Visual DAT relocation target is outside its data block."; return false;
    }
    out->relocations.push_back(offset);
  }
  for (uint32_t i = 0; i < roots + references; ++i) {
    uint64_t record = root_start + (uint64_t)i * 8;
    uint32_t object = be32(bytes.data() + record), name = be32(bytes.data() + record + 4);
    if (object >= data_size || name >= bytes.size() - strings) {
      *error = "Visual DAT root/reference points outside its tables."; return false;
    }
    size_t cursor = (size_t)strings + name, begin = cursor, count = 0;
    while (cursor < bytes.size() && bytes[cursor] && count++ <= 255) {
      if (bytes[cursor] < 0x20 || bytes[cursor] > 0x7e) {
        *error = "Visual DAT symbol is not printable ASCII."; return false;
      }
      ++cursor;
    }
    if (cursor == bytes.size() || count > 256) {
      *error = "Visual DAT symbol is not bounded and terminated."; return false;
    }
    if (i < roots) out->roots.emplace_back((const char*)bytes.data() + begin, cursor - begin);
  }
  for (uint32_t offset : out->relocations) {
    if ((uint64_t)offset + 24 > data_size) continue;
    const uint8_t* descriptor = bytes.data() + 0x20ull + offset;
    VisualImage image;
    image.target = be32(descriptor); image.width = (uint16_t)(be32(descriptor + 4) >> 16);
    image.height = (uint16_t)be32(descriptor + 4);
    image.format = be32(descriptor + 8); image.mipmap = be32(descriptor + 12);
    image.min_lod_bits = be32(descriptor + 16); image.max_lod_bits = be32(descriptor + 20);
    float min_lod = 0, max_lod = 0;
    std::memcpy(&min_lod, &image.min_lod_bits, 4); std::memcpy(&max_lod, &image.max_lod_bits, 4);
    if (!image.target || !image.width || !image.height || image.width > 4096 || image.height > 4096 ||
        image.mipmap > 1 || !std::isfinite(min_lod) || !std::isfinite(max_lod) ||
        min_lod < 0 || max_lod < min_lod || max_lod > 12 ||
        (!image.mipmap && (min_lod != 0 || max_lod != 0))) continue;
    image.size = gx_image_size(image.width, image.height, image.format, image.mipmap, max_lod);
    if (!image.size || (uint64_t)image.target + image.size > data_size) continue;
    out->images[offset] = image;
  }
  if (out->images.empty()) { *error = "Visual DAT has no validated GX image descriptors."; return false; }
  // HSD_TObjDesc stores its image and TLUT descriptor pointers at +0x4c and +0x50. Only accept a
  // palette descriptor reached through a pointer to an already validated image descriptor.
  for (uint32_t image_pointer : out->relocations) {
    uint32_t image_descriptor = be32(bytes.data() + 0x20ull + image_pointer);
    if (image_pointer < 0x4c || out->images.find(image_descriptor) == out->images.end()) continue;
    uint32_t palette_pointer = image_pointer + 4;
    if (std::find(out->relocations.begin(), out->relocations.end(), palette_pointer) ==
        out->relocations.end()) continue;
    uint32_t descriptor = be32(bytes.data() + 0x20ull + palette_pointer);
    if (!descriptor || (uint64_t)descriptor + 16 > data_size ||
        std::find(out->relocations.begin(), out->relocations.end(), descriptor) ==
            out->relocations.end()) continue;
    const uint8_t* palette = bytes.data() + 0x20ull + descriptor;
    VisualPalette item;
    item.target = be32(palette); item.format = be32(palette + 4); item.name = be32(palette + 8);
    item.entries = (uint16_t)(be32(palette + 12) >> 16); item.size = (uint32_t)item.entries * 2;
    if ((item.format > 2) ||
        (item.entries != 16 && item.entries != 256 && item.entries != 16384) ||
        (uint64_t)item.target + item.size > data_size) continue;
    out->palettes[descriptor] = item;
  }
  return true;
}

// A costume's skeleton places the fighter's hurtboxes and hitboxes, so online it must match the
// vanilla slot exactly: same joints, same hierarchy, same rest transforms, same transform flags.
// Flags that only affect drawing (hidden, lighting, texgen, specular, opaque/translucent classes)
// may differ. Anything else and the costume stays offline; online the vanilla model is loaded.
namespace skeleton {
// 0x1, 0x2 and 0x4 (skeleton, skeleton root, envelope model) only say how the mesh is skinned for
// drawing. Model tools rewrite them on export, which kept every re-exported skin off online.
// 0x8 (classical scaling) is not the costume's to decide: every time an animation is put on a
// fighter joint the game sets or clears that bit from the animation itself (lbanim.c), so the value
// in the file does not last. Model tools set it on every joint when they export, and that alone
// kept nine of the ten rejected costumes on a widely used training pack off online.
constexpr uint32_t kVisualFlags = 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x10000u |
                                  0x40000u | 0x80000u | 0x100000u | 0x70000000u;
// A skin exported from a model tool carries the same rest pose with different rounding in the last
// digits, and a bit-for-bit comparison refused it. The nine values (rotation, scale, translation)
// are compared as numbers with a small tolerance; a real change of a joint is far larger.
// Returns which parts of the rest pose differ (0 when none do), so the reason can say what changed.
constexpr uint32_t kRestRotated = 1, kRestResized = 2, kRestMoved = 4;
inline uint32_t rest_pose_difference(const uint8_t* p, const uint8_t* q) {
  uint32_t differs = 0;
  for (int i = 0; i < 9; ++i) {
    uint32_t ua = be32(p + i * 4), ub = be32(q + i * 4);
    float fa, fb; std::memcpy(&fa, &ua, 4); std::memcpy(&fb, &ub, 4);
    if (ua == ub) continue;
    const uint32_t part = i < 3 ? kRestRotated : i < 6 ? kRestResized : kRestMoved;
    if (!std::isfinite(fa) || !std::isfinite(fb)) { differs |= part; continue; }
    const float scale = std::max(1.0f, std::max(std::fabs(fa), std::fabs(fb)));
    // The first three values are the rest rotation, which a fighter's animations replace on every
    // frame, so an exporter's rounding there gets more room than in scale and translation.
    const float tolerance = i < 3 ? 1e-2f : 1e-3f;
    if (std::fabs(fa - fb) > tolerance * scale) differs |= part;
  }
  return differs;
}
// The game's rest rotation matrix of a joint (HSD_MtxSRT: Rz * Ry * Rx, scale left out), for
// same_rest_placement. Double precision: only the comparison needs it, nothing is written back.
void rest_rotation(const uint8_t* p, double m[3][3]) {
  double r[3];
  for (int i = 0; i < 3; ++i) { const uint32_t u = be32(p + i * 4); float f; std::memcpy(&f, &u, 4); r[i] = f; }
  const double cx = std::cos(r[0]), sx = std::sin(r[0]), cy = std::cos(r[1]), sy = std::sin(r[1]),
               cz = std::cos(r[2]), sz = std::sin(r[2]);
  m[0][0] = cy * cz; m[0][1] = sx * sy * cz - cx * sz; m[0][2] = cx * sy * cz + sx * sz;
  m[1][0] = cy * sz; m[1][1] = sx * sy * sz + cx * cz; m[1][2] = cx * sy * sz - sx * cz;
  m[2][0] = -sy;     m[2][1] = sx * cy;                m[2][2] = cx * cy;
}
bool same_rest_placement(const uint8_t* p, const uint8_t* q) {
  const uint32_t differs = rest_pose_difference(p, q);
  if (differs & (kRestResized | kRestMoved)) return false;
  if (!(differs & kRestRotated)) return true;
  for (int i = 0; i < 3; ++i) {
    const uint32_t ua = be32(p + i * 4), ub = be32(q + i * 4);
    float fa, fb; std::memcpy(&fa, &ua, 4); std::memcpy(&fb, &ub, 4);
    if (!std::isfinite(fa) || !std::isfinite(fb)) return false;
  }
  double a[3][3], b[3][3];
  rest_rotation(p, a); rest_rotation(q, b);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      if (std::fabs(a[r][c] - b[r][c]) > 1e-2) return false;
  return true;
}
// `relocated` receives the data offsets the relocation table lists, sorted. A pointer in this format
// is an offset into the data block, so a joint stored at data offset 0 is pointed to by the value 0:
// only the relocation table tells that pointer from "no joint".
bool share_joint_root(const std::vector<uint8_t>& bytes, uint32_t* data_size, uint32_t* root,
                      std::vector<uint32_t>* relocated, std::string* error) {
  if (bytes.size() < 0x20) { *error = "DAT header is truncated."; return false; }
  const uint64_t dsize = be32(bytes.data() + 4), relocations = be32(bytes.data() + 8),
                 roots = be32(bytes.data() + 12), references = be32(bytes.data() + 16);
  const uint64_t root_table = 0x20ull + dsize + relocations * 4ull;
  const uint64_t strings = root_table + (roots + references) * 8ull;
  if (roots > 1024 || references > 1024 || strings > bytes.size()) { *error = "DAT tables point outside the file."; return false; }
  relocated->resize((size_t)relocations);
  for (uint64_t i = 0; i < relocations; ++i) (*relocated)[(size_t)i] = be32(&bytes[(size_t)(0x20ull + dsize + i * 4ull)]);
  std::sort(relocated->begin(), relocated->end());
  for (uint64_t i = 0; i < roots; ++i) {
    const uint8_t* record = &bytes[(size_t)(root_table + i * 8)];
    const uint64_t name = strings + be32(record + 4);
    if (name >= bytes.size()) continue;
    const char* s = (const char*)&bytes[(size_t)name];
    const size_t n = strnlen(s, bytes.size() - (size_t)name);
    static constexpr char suffix[] = "_Share_joint";
    if (n >= sizeof suffix - 1 && std::memcmp(s + n - (sizeof suffix - 1), suffix, sizeof suffix - 1) == 0) {
      *data_size = (uint32_t)dsize; *root = be32(record);
      if (*root + 0x40ull > dsize) { *error = "Skeleton root points outside the data block."; return false; }
      return true;
    }
  }
  *error = "No _Share_joint skeleton root."; return false;
}
inline bool is_pointer(const std::vector<uint32_t>& relocated, uint32_t slot) {
  return std::binary_search(relocated.begin(), relocated.end(), slot);
}
// The number of joints in one tree, stopping past 1024 (a cycle or a broken file). Only used to
// word the reason once the two trees are known to differ in shape.
uint32_t count_joints(const std::vector<uint8_t>& bytes, uint32_t size, const std::vector<uint32_t>& relocated,
                      uint32_t root) {
  uint32_t count = 0;
  std::vector<uint32_t> stack{root};
  while (!stack.empty() && count <= 1024) {
    const uint32_t x = stack.back(); stack.pop_back();
    if (x + 0x40ull > size) continue;
    ++count;
    const uint8_t* p = &bytes[0x20ull + x];
    if (is_pointer(relocated, x + 0x0C)) stack.push_back(be32(p + 0x0C));
    if (is_pointer(relocated, x + 0x08)) stack.push_back(be32(p + 0x08));
  }
  return count;
}
// `a` is the standard file, `b` the costume. Joints are numbered depth first (child, then next),
// from 0 at the root, and every refusal names the joint so the player can find it in a model tool.
bool same_tree(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
               const std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel,
               uint32_t* joints, std::string* error, bool any_rest_pose = false, bool any_flags = false) {
  // any_rest_pose: the rest pose is not compared at all (the caller copies the standard one over it).
  // any_flags: nor are the joints' flags (the desktop app's Slippi-safe rule does not look at them).
  // Iterative walk of both trees in lockstep (child first, then next), bounded against cycles.
  // A child or next slot holds a joint only when the relocation table lists the slot: the value 0
  // is a real pointer to a joint stored at data offset 0, and reading it as "no joint" called a
  // matching skeleton a different shape.
  struct Pair { uint32_t x, y; bool has_x, has_y; };
  std::vector<Pair> stack{{ja, jb, true, true}};
  while (!stack.empty()) {
    const Pair top = stack.back(); stack.pop_back();
    const uint32_t x = top.x, y = top.y;
    if (!top.has_x || !top.has_y) {
      if (top.has_x || top.has_y) {
        const uint32_t standard = count_joints(a, asize, arel, ja), costume = count_joints(b, bsize, brel, jb);
        if (standard != costume)
          *error = "Skeleton joint count differs from the vanilla costume (" + std::to_string(costume) +
                   " instead of " + std::to_string(standard) + ").";
        else
          *error = "Skeleton joint " + std::to_string(*joints) + " hierarchy differs from the vanilla costume.";
        return false;
      }
      continue;
    }
    if (x + 0x40ull > asize || y + 0x40ull > bsize) { *error = "Skeleton joint points outside the data block."; return false; }
    if (++*joints > 1024) { *error = "Skeleton has more than 1024 joints."; return false; }
    const uint8_t* p = &a[0x20ull + x];
    const uint8_t* q = &b[0x20ull + y];
    if (!any_flags && (be32(p + 4) & ~kVisualFlags) != (be32(q + 4) & ~kVisualFlags)) {
      *error = "Skeleton joint " + std::to_string(*joints - 1) + " flags differ from the vanilla costume."; return false;
    }
    if (const uint32_t differs = any_rest_pose ? 0u : rest_pose_difference(p + 0x14, q + 0x14)) {
      std::string what;
      if (differs & kRestRotated) what += "rotated";
      if (differs & kRestResized) what += what.empty() ? "resized" : ", resized";
      if (differs & kRestMoved) what += what.empty() ? "moved" : ", moved";
      *error = "Skeleton joint " + std::to_string(*joints - 1) + " rest pose differs from the vanilla costume (" + what + ").";
      return false;
    }
    stack.push_back({be32(p + 0x0C), be32(q + 0x0C), is_pointer(arel, x + 0x0C), is_pointer(brel, y + 0x0C)});   // next sibling
    stack.push_back({be32(p + 0x08), be32(q + 0x08), is_pointer(arel, x + 0x08), is_pointer(brel, y + 0x08)});   // first child
  }
  return true;
}
// The game stops (pobj.c, assertion "jp->envelopemtx") when it draws an envelope mesh whose blended
// matrix names a joint that has no inverse bind matrix: the joint description's pointer at 0x38 is
// what the loader copies into the joint, and a blend of two or more joints multiplies by it for
// every joint in the list. A console stops the same way, so such a costume is never handed to the
// game. A list whose first weight is 1 takes the game's unchecked path and is left alone here.
// Only a certain defect refuses: anything that points outside the data block is skipped, since the
// other checks own malformed files.
bool envelopes_bound(const std::vector<uint8_t>& bytes, uint32_t size, const std::vector<uint32_t>& relocated,
                     uint32_t root, std::string* error) {
  constexpr uint32_t kNoMeshFlags = 0x20u | 0x4000u;   // particle and spline joints keep other data in the mesh slot
  constexpr size_t kMaxWalk = 65536;                   // against cycles in a broken file
  std::vector<uint32_t> order;                         // joints depth first (child, then next), as same_tree numbers them
  std::vector<uint32_t> stack{root};
  while (!stack.empty() && order.size() <= 1024) {
    const uint32_t x = stack.back(); stack.pop_back();
    if (x + 0x40ull > size) continue;
    order.push_back(x);
    const uint8_t* p = &bytes[0x20ull + x];
    if (is_pointer(relocated, x + 0x0C)) stack.push_back(be32(p + 0x0C));
    if (is_pointer(relocated, x + 0x08)) stack.push_back(be32(p + 0x08));
  }
  if (order.size() > 1024) return true;
  size_t walked = 0;
  for (const uint32_t x : order) {
    const uint8_t* p = &bytes[0x20ull + x];
    if ((be32(p + 4) & kNoMeshFlags) || !is_pointer(relocated, x + 0x10)) continue;
    for (uint32_t d = be32(p + 0x10);; d = be32(&bytes[0x20ull + d + 0x04])) {          // display objects
      if (d + 0x10ull > size || ++walked > kMaxWalk) break;
      if (is_pointer(relocated, d + 0x0C))
        for (uint32_t o = be32(&bytes[0x20ull + d + 0x0C]);; o = be32(&bytes[0x20ull + o + 0x04])) {   // polygon objects
          if (o + 0x18ull > size || ++walked > kMaxWalk) break;
          const uint8_t* po = &bytes[0x20ull + o];
          const uint32_t type = (((uint32_t)po[0x0C] << 8) | po[0x0D]) & 0x3000u;
          if (type == 0x2000u && is_pointer(relocated, o + 0x14)) {
            // The envelope list: pointers to arrays of (joint, weight), each ended by a slot that is no pointer.
            for (uint64_t slot = be32(po + 0x14); slot + 4 <= size && is_pointer(relocated, (uint32_t)slot); slot += 4) {
              if (++walked > kMaxWalk) break;
              const uint64_t first = be32(&bytes[(size_t)(0x20ull + slot)]);
              if (first + 8 > size || !is_pointer(relocated, (uint32_t)first)) continue;
              const uint32_t bits = be32(&bytes[(size_t)(0x20ull + first + 4)]);
              float weight; std::memcpy(&weight, &bits, 4);
              if (weight >= 1.0f - FLT_EPSILON) continue;
              for (uint64_t e = first; e + 8 <= size && is_pointer(relocated, (uint32_t)e); e += 8) {
                if (++walked > kMaxWalk) break;
                const uint32_t joint = be32(&bytes[(size_t)(0x20ull + e)]);
                if (joint + 0x40ull > size || is_pointer(relocated, joint + 0x38)) continue;
                const auto found = std::find(order.begin(), order.end(), joint);
                *error = "A mesh is skinned to bone " +
                         (found == order.end() ? "at " + std::to_string(joint) : std::to_string(found - order.begin())) +
                         ", which has no bind matrix: the game would stop when it is drawn.";
                return false;
              }
            }
          }
          if (!is_pointer(relocated, o + 0x04)) break;
        }
      if (!is_pointer(relocated, d + 0x04)) break;
    }
  }
  return true;
}
// The simulation reads a fighter's bones from its costume file: every joint's rest rotation, scale
// and translation feeds the bone matrices that hurtboxes, joint-bound hitboxes and the ECB follow
// wherever an animation does not drive that channel. The game copies the costume's rest values into
// every part again at each action change (ftAnim_8006EBE8 -> ftAnim_8006FA58 -> lb_8000B4FC) and the
// action's animation then overwrites only the channels it has tracks for (Mario's joint 30 has no
// rotation track in 193 of his 195 actions). A skin whose rest pose is within the online check's
// tolerance but not bit-equal therefore moved hurtboxes on one side of an online match only.
// After same_tree has matched the two trees joint for joint, this writes the standard file's rest
// values (the nine floats at joint +0x14) into the skin's copy, so the skin is drawn with its own
// meshes, materials and textures over bones identical to the standard costume's.
uint32_t copy_rest_pose(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
                        std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel) {
  struct Pair { uint32_t x, y; };
  std::vector<Pair> stack{{ja, jb}};
  uint32_t changed = 0, joints = 0;
  while (!stack.empty() && joints < 1024) {
    const Pair top = stack.back(); stack.pop_back();
    const uint32_t x = top.x, y = top.y;
    if (x + 0x40ull > asize || y + 0x40ull > bsize) return changed;
    ++joints;
    const uint8_t* p = &a[0x20ull + x];
    uint8_t* q = &b[0x20ull + y];
    if (std::memcmp(p + 0x14, q + 0x14, 0x24) != 0) { std::memcpy(q + 0x14, p + 0x14, 0x24); ++changed; }
    if (is_pointer(arel, x + 0x0C) && is_pointer(brel, y + 0x0C)) stack.push_back({be32(p + 0x0C), be32(q + 0x0C)});
    if (is_pointer(arel, x + 0x08) && is_pointer(brel, y + 0x08)) stack.push_back({be32(p + 0x08), be32(q + 0x08)});
  }
  return changed;
}
// What the joint loader reads from a joint description besides the tree, the flags and the rest pose
// (jobj.c HSD_JObjLoadJoint, checked 2026-11-25):
//   +0x00, the class name: a name the game has registered builds the joint as that class
//   (hsdSearchClassInfo, then hsdNew), no name a plain joint (HSD_JObjAlloc), and a value the relocation
//   table does not list is read as an address all the same. So it can change how the joint is built.
//   Retail costume joints name no class.
//   +0x3C, the constraint (robj, HSD_RObjLoadDesc), which moves the bone. Retail costume joints have none.
// After same_tree matched the two trees, this walks them in lockstep (numbered as same_tree does):
// `class_names`: each costume joint's +0x00 is what the standard joint's is, both no pointer and the
// same four bytes, or both pointers to the same text. `constraints`: a costume joint's +0x3C is what the
// standard joint's is, no pointer and the same four bytes (a constraint is refused).
bool same_joint_build(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
                      const std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel,
                      bool class_names, bool constraints, std::string* error) {
  auto text = [](const std::vector<uint8_t>& bytes, uint32_t size, uint32_t at, std::string* out) {
    if (at >= size) return false;
    const char* s = (const char*)&bytes[0x20ull + at];
    const size_t n = strnlen(s, size - at);
    if (n == size - at) return false;   // not ended inside the data block
    out->assign(s, n);
    return true;
  };
  struct Pair { uint32_t x, y; };
  std::vector<Pair> stack{{ja, jb}};
  uint32_t joint = 0;
  while (!stack.empty() && joint < 1024) {
    const Pair top = stack.back(); stack.pop_back();
    const uint32_t x = top.x, y = top.y;
    if (x + 0x40ull > asize || y + 0x40ull > bsize) { *error = "Skeleton joint points outside the data block."; return false; }
    const uint8_t* p = &a[0x20ull + x];
    const uint8_t* q = &b[0x20ull + y];
    if (class_names) {
      const bool named = is_pointer(arel, x);
      bool same = named == is_pointer(brel, y);
      if (same && !named) same = std::memcmp(p, q, 4) == 0;
      std::string s, t;
      if (same && named) same = text(a, asize, be32(p), &s) && text(b, bsize, be32(q), &t) && s == t;
      if (!same) {
        *error = "Skeleton joint " + std::to_string(joint) +
                 " class name differs from the vanilla costume (the game builds a joint by its class).";
        return false;
      }
    }
    if (constraints && (is_pointer(arel, x + 0x3C) || is_pointer(brel, y + 0x3C) || std::memcmp(p + 0x3C, q + 0x3C, 4) != 0)) {
      *error = "Skeleton joint " + std::to_string(joint) + " carries a constraint (robj); vanilla costume joints have none.";
      return false;
    }
    ++joint;
    if (is_pointer(arel, x + 0x0C) && is_pointer(brel, y + 0x0C)) stack.push_back({be32(p + 0x0C), be32(q + 0x0C)});
    if (is_pointer(arel, x + 0x08) && is_pointer(brel, y + 0x08)) stack.push_back({be32(p + 0x08), be32(q + 0x08)});
  }
  return true;
}
// Every root of an archive (symbol, data offset) and its relocation list, sorted. False when the
// tables lie outside the file.
bool root_table(const std::vector<uint8_t>& bytes, uint32_t* data_size, std::vector<uint32_t>* relocated,
                std::vector<std::pair<std::string, uint32_t>>* roots, std::string* error) {
  if (bytes.size() < 0x20) { *error = "DAT header is truncated."; return false; }
  const uint64_t dsize = be32(bytes.data() + 4), relocations = be32(bytes.data() + 8),
                 count = be32(bytes.data() + 12), references = be32(bytes.data() + 16);
  const uint64_t table = 0x20ull + dsize + relocations * 4ull, strings = table + (count + references) * 8ull;
  if (count > 1024 || references > 1024 || strings > bytes.size()) { *error = "DAT tables point outside the file."; return false; }
  relocated->resize((size_t)relocations);
  for (uint64_t i = 0; i < relocations; ++i) (*relocated)[(size_t)i] = be32(&bytes[(size_t)(0x20ull + dsize + i * 4ull)]);
  std::sort(relocated->begin(), relocated->end());
  roots->clear();
  for (uint64_t i = 0; i < count; ++i) {
    const uint8_t* record = &bytes[(size_t)(table + i * 8)];
    const uint64_t name = strings + be32(record + 4);
    if (name >= bytes.size()) continue;
    const char* s = (const char*)&bytes[(size_t)name];
    roots->emplace_back(std::string(s, strnlen(s, bytes.size() - (size_t)name)), be32(record));
  }
  *data_size = (uint32_t)dsize;
  return true;
}
// A skeleton root by the desktop app's rule: a symbol ending in "_joint" that is not a material or
// animation joint (Jigglypuff's hats are skeleton roots too).
bool skeleton_root_name(const std::string& s) {
  static constexpr char tail[] = "_joint";
  return s.size() >= sizeof tail - 1 && s.compare(s.size() - (sizeof tail - 1), sizeof tail - 1, tail) == 0 &&
         s.find("matanim") == std::string::npos && s.find("animjoint") == std::string::npos;
}
}  // namespace skeleton

// A costume skin in a disc slot (a mod disc, a pack, an L / R pick), made the standard costume's bones:
// every skeleton root of the slot's standard file walks in lockstep with the skin's root of the same
// name, and the skin's copy takes the standard rest pose on every joint of them, whatever the
// difference (the desktop app's fix, SSBM Nucleus costume_validator.py). Refused when the fix cannot be
// made or the game could not draw the result: a root the game asks for is missing, a tree does not line
// up with the standard one, or a mesh is skinned to a bone with no bind matrix.
// `peer_rule` (online, 2026-11-25): also everything else the peer rule compares that the simulation
// reads or that decides whether a skin is shown (host::skins::peer_skin_policy, costume_served_form.h),
// so this machine never simulates online with a skin an opponent's machine would refuse: the non-visual
// joint flags, a constraint (robj), the class name, and a joint root the standard file does not have.
// The rest of that rule (one costume of the slot's fighter and colour, the display object limits, the
// importer's structure rule) a skin in a disc slot has passed at import. Without it (offline), as before.
// `fixed` (may be null) gets the fixed copy; `detail` says what was found.
static bool fix_rest_pose(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& skin,
                          std::vector<uint8_t>* fixed, std::string* detail, bool peer_rule) {
  uint32_t asize = 0, bsize = 0, joints = 0, changed = 0, trees = 0;
  std::vector<uint32_t> arel, brel;
  std::vector<std::pair<std::string, uint32_t>> aroots, broots;
  if (!skeleton::root_table(clean, &asize, &arel, &aroots, detail) || !skeleton::root_table(skin, &bsize, &brel, &broots, detail))
    return false;
  if (asize + 0x20ull > clean.size() || bsize + 0x20ull > skin.size()) { *detail = "DAT data block is truncated."; return false; }
  for (const auto& root : broots) {
    static constexpr char tail[] = "_joint";
    const bool joint_root = root.first.size() >= sizeof tail - 1 &&
                            root.first.compare(root.first.size() - (sizeof tail - 1), sizeof tail - 1, tail) == 0;
    if (!peer_rule || !joint_root) continue;
    if (std::none_of(aroots.begin(), aroots.end(), [&](const auto& r) { return r.first == root.first; })) {
      *detail = "Unknown joint root " + root.first + " (the standard file has no such root).";
      return false;
    }
  }
  std::vector<uint8_t> copy = skin;
  for (const auto& root : aroots) {
    if (!skeleton::skeleton_root_name(root.first)) continue;
    const auto mine = std::find_if(broots.begin(), broots.end(), [&](const auto& r) { return r.first == root.first; });
    if (mine == broots.end()) { *detail = "No " + root.first + " root (the game asks for it)."; return false; }
    if (root.second + 0x40ull > asize || mine->second + 0x40ull > bsize) { *detail = "Skeleton root points outside the data block."; return false; }
    uint32_t walked = 0;
    if (!skeleton::same_tree(clean, asize, root.second, arel, skin, bsize, mine->second, brel, &walked, detail,
                             /*any_rest_pose=*/true, /*any_flags=*/!peer_rule))
      return false;
    if (peer_rule && !skeleton::same_joint_build(clean, asize, root.second, arel, skin, bsize, mine->second, brel, true, true, detail))
      return false;
    if (!skeleton::envelopes_bound(skin, bsize, brel, mine->second, detail)) return false;
    joints += walked;
    changed += skeleton::copy_rest_pose(clean, asize, root.second, arel, copy, bsize, mine->second, brel);
    ++trees;
  }
  if (!trees) { *detail = "No skeleton root in the standard file."; return false; }
  *detail = std::to_string(joints) + " joints match" +
            (changed ? ", " + std::to_string(changed) + " take the standard rest pose" : std::string());
  if (fixed) *fixed = std::move(copy);
  return true;
}
bool costume_fix_rest_pose(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& skin,
                           std::vector<uint8_t>* fixed, std::string* detail) {
  return fix_rest_pose(clean, skin, fixed, detail, false);
}
bool costume_online_fix(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& skin,
                        std::vector<uint8_t>* fixed, std::string* detail) {
  return fix_rest_pose(clean, skin, fixed, detail, true);
}

// The served copy of an online-allowed costume skin gets the standard costume's joint rest pose
// (skeleton::copy_rest_pose). Returns how many joints differed; 0 when nothing had to change.
[[maybe_unused]] uint32_t costume_take_standard_bones(const std::vector<uint8_t>& clean, std::vector<uint8_t>& served) {
  uint32_t asize = 0, aroot = 0, bsize = 0, broot = 0, joints = 0;
  std::vector<uint32_t> arel, brel;
  std::string error;
  if (!skeleton::share_joint_root(clean, &asize, &aroot, &arel, &error) ||
      !skeleton::share_joint_root(served, &bsize, &broot, &brel, &error)) return 0;
  if (asize + 0x20ull > clean.size() || bsize + 0x20ull > served.size()) return 0;
  if (!skeleton::same_tree(clean, asize, aroot, arel, served, bsize, broot, brel, &joints, &error)) return 0;
  return skeleton::copy_rest_pose(clean, asize, aroot, arel, served, bsize, broot, brel);
}

}  // namespace cosmetic_internal

// Public: whether the game can draw this costume at all. Offline and online alike, unlike the
// skeleton comparison below: a file that fails here stops the game on any machine.
bool costume_draw_safe(const std::vector<uint8_t>& candidate, std::string* error) {
  std::string local; if (!error) error = &local;
  uint32_t size = 0, root = 0;
  std::vector<uint32_t> relocated;
  std::string unused;
  // No skeleton root or a truncated file is some other check's finding, not a drawing defect.
  if (!skeleton::share_joint_root(candidate, &size, &root, &relocated, &unused) || size + 0x20ull > candidate.size()) return true;
  return skeleton::envelopes_bound(candidate, size, relocated, root, error);
}

// Public: the same question for a stage file. A report (0.8.80, All-Star with a custom stage
// installed) stopped on the same assertion with every costume already checked: a stage's models
// are drawn by the same code and were never looked at. The stage's "map_head" root lists its
// models, 0x34 bytes each with the joint tree first (gr/types.h UnkStageDat).
bool stage_draw_safe(const std::vector<uint8_t>& candidate, std::string* error) {
  std::string local; if (!error) error = &local;
  if (candidate.size() < 0x20) return true;
  const uint64_t size = be32(candidate.data() + 4), relocations = be32(candidate.data() + 8),
                 roots = be32(candidate.data() + 12), references = be32(candidate.data() + 16);
  const uint64_t root_table = 0x20ull + size + relocations * 4ull;
  const uint64_t strings = root_table + (roots + references) * 8ull;
  // A truncated or oversized table is some other check's finding, not a drawing defect.
  if (roots > 1024 || references > 1024 || relocations > candidate.size() / 4 || strings > candidate.size()) return true;
  std::vector<uint32_t> relocated((size_t)relocations);
  for (uint64_t i = 0; i < relocations; ++i) relocated[(size_t)i] = be32(&candidate[(size_t)(0x20ull + size + i * 4ull)]);
  std::sort(relocated.begin(), relocated.end());
  for (uint64_t i = 0; i < roots; ++i) {
    const uint8_t* record = &candidate[(size_t)(root_table + i * 8)];
    const uint64_t name = strings + be32(record + 4);
    if (name >= candidate.size()) continue;
    const char* text = (const char*)&candidate[(size_t)name];
    if (strnlen(text, candidate.size() - (size_t)name) != 8 || std::memcmp(text, "map_head", 8) != 0) continue;
    const uint64_t head = be32(record);
    if (head + 0x10 > size || !skeleton::is_pointer(relocated, (uint32_t)head + 8)) return true;
    const uint64_t models = be32(&candidate[(size_t)(0x20ull + head + 8)]);
    const uint64_t count = be32(&candidate[(size_t)(0x20ull + head + 0x0C)]);
    for (uint64_t m = 0; m < count && m < 1024; ++m) {
      const uint64_t entry = models + m * 0x34ull;
      if (entry + 0x34 > size) break;
      if (!skeleton::is_pointer(relocated, (uint32_t)entry)) continue;   // a model slot with no joint tree
      const uint32_t tree = be32(&candidate[(size_t)(0x20ull + entry)]);
      if (tree + 0x40ull > size) continue;
      if (!skeleton::envelopes_bound(candidate, (uint32_t)size, relocated, tree, error)) return false;
    }
  }
  return true;
}

// Public: the Source Port asks the same question about a live pack's costume files.
bool costume_skeleton_matches(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                              std::string* error) {
  std::string local; if (!error) error = &local;
  uint32_t asize = 0, aroot = 0, bsize = 0, broot = 0, joints = 0;
  std::vector<uint32_t> arel, brel;
  if (!skeleton::share_joint_root(clean, &asize, &aroot, &arel, error) ||
      !skeleton::share_joint_root(candidate, &bsize, &broot, &brel, error)) return false;
  if (asize + 0x20ull > clean.size() || bsize + 0x20ull > candidate.size()) { *error = "DAT data block is truncated."; return false; }
  if (!skeleton::same_tree(clean, asize, aroot, arel, candidate, bsize, broot, brel, &joints, error)) return false;
  *error = std::to_string(joints) + " joints match";
  return true;
}

// Public (costume_skeleton.h): the skeleton code above for any pair of joint roots, so an
// opponent's skin (costume_served_form.cpp) is checked by the same walk on every skeleton root.
namespace skeleton_shared {
bool same_tree(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
               const std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel,
               bool any_rest_pose, uint32_t* joints, std::string* error, bool any_flags) {
  return skeleton::same_tree(a, asize, ja, arel, b, bsize, jb, brel, joints, error, any_rest_pose, any_flags);
}
uint32_t copy_rest_pose(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
                        std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel) {
  return skeleton::copy_rest_pose(a, asize, ja, arel, b, bsize, jb, brel);
}
uint32_t rest_pose_difference(const uint8_t* standard, const uint8_t* costume) {
  return skeleton::rest_pose_difference(standard, costume);
}
bool same_rest_placement(const uint8_t* standard, const uint8_t* costume) {
  return skeleton::same_rest_placement(standard, costume);
}
bool envelopes_bound(const std::vector<uint8_t>& bytes, uint32_t size, const std::vector<uint32_t>& relocated,
                     uint32_t root, std::string* error) {
  return skeleton::envelopes_bound(bytes, size, relocated, root, error);
}
bool same_joint_build(const std::vector<uint8_t>& a, uint32_t asize, uint32_t ja, const std::vector<uint32_t>& arel,
                      const std::vector<uint8_t>& b, uint32_t bsize, uint32_t jb, const std::vector<uint32_t>& brel,
                      bool class_names, bool constraints, std::string* error) {
  return skeleton::same_joint_build(a, asize, ja, arel, b, bsize, jb, brel, class_names, constraints, error);
}
}  // namespace skeleton_shared

// Public: the rule a disc slot's costume skin plays online by (cosmetic_internal::costume_online_fix).
bool costume_online_form(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                         std::vector<uint8_t>* fixed, std::string* detail) {
  std::string local; if (!detail) detail = &local;
  return cosmetic_internal::costume_online_fix(clean, candidate, fixed, detail);
}

// The player reads this beside "Online: standard costume", so it only has to name the difference.
// The full sentence stays in the log.
std::string online_reason_short(const std::string& detail) {
  if (detail.empty()) return "the standard file could not be read";
  const size_t incomplete = detail.rfind("incomplete visual conversion (");
  if (incomplete != std::string::npos) return detail.substr(incomplete);
  if (detail.find("joints match") != std::string::npos) return detail;
  // "Skeleton joint 12 ... (moved)." names the bone, so a player knows which one to put back, and
  // the bracket (what changed, or the two joint counts) is carried over as written.
  const size_t open = detail.find('('), close = detail.find(')');
  const std::string bracket = open != std::string::npos && close != std::string::npos && close > open
                                  ? " " + detail.substr(open, close - open + 1) : std::string();
  static constexpr char kJoint[] = "Skeleton joint ";
  constexpr size_t kJointLength = sizeof kJoint - 1;
  if (detail.find("joint count differs") != std::string::npos) return "bone count differs" + bracket;
  if (detail.compare(0, kJointLength, kJoint) == 0) {
    size_t end = kJointLength;
    while (end < detail.size() && std::isdigit((unsigned char)detail[end])) ++end;
    if (end > kJointLength) {
      const std::string bone = "bone " + detail.substr(kJointLength, end - kJointLength) + ": ";
      if (detail.find("rest pose differs") != std::string::npos) return bone + "rest pose differs" + bracket;
      if (detail.find("flags differ") != std::string::npos) return bone + "settings differ";
      if (detail.find("hierarchy differs") != std::string::npos) return bone + "shape differs";
      if (detail.find("class name differs") != std::string::npos) return bone + "class differs";
      if (detail.find("carries a constraint") != std::string::npos) return bone + "has a constraint";
    }
  }
  if (detail.find("Unknown joint root") != std::string::npos) return "extra joint root";
  if (detail.find("more than 1024 joints") != std::string::npos) return "skeleton too large";
  if (detail.find("No _Share_joint") != std::string::npos) return "no skeleton found";
  if (detail.find("non-texture data") != std::string::npos) return "changes more than textures";
  if (detail.find("changes layout") != std::string::npos) return "changes the file layout";
  if (detail.find("identical to the clean resource") != std::string::npos) return "same as the standard file";
  std::string text = detail;
  while (!text.empty() && (text.back() == '.' || text.back() == ' ')) text.pop_back();
  return text;
}

namespace cosmetic_internal {

bool visual_dat_only(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                     std::string* error) {
  VisualLayout before, after;
  if (!parse_visual_layout(clean, &before, error) || !parse_visual_layout(candidate, &after, error))
    return false;
  if (clean.size() != candidate.size() || before.data_size != after.data_size ||
      before.relocations != after.relocations || before.images != after.images ||
      before.palettes != after.palettes ||
      before.relocation_start != after.relocation_start ||
      std::memcmp(clean.data() + before.relocation_start, candidate.data() + after.relocation_start,
                  clean.size() - before.relocation_start)) {
    *error = "Visual DAT changes layout, relocation, root, string, or GX descriptor structure.";
    return false;
  }
  std::vector<uint8_t> allowed(before.data_size, 0);
  for (const auto& pair : before.images)
    std::fill(allowed.begin() + pair.second.target,
              allowed.begin() + pair.second.target + pair.second.size, 1);
  for (const auto& pair : before.palettes)
    std::fill(allowed.begin() + pair.second.target,
              allowed.begin() + pair.second.target + pair.second.size, 1);
  size_t changed = 0;
  for (uint32_t offset = 0; offset < before.data_size; ++offset) {
    if (clean[0x20ull + offset] == candidate[0x20ull + offset]) continue;
    ++changed;
    if (!allowed[offset]) {
      std::ostringstream message;
      message << "Visual DAT changes non-texture data at data offset 0x" << std::hex << offset << ".";
      *error = message.str(); return false;
    }
  }
  if (!changed) { *error = "Visual DAT is identical to the clean resource."; return false; }
  return true;
}

// The game's pause loader indexes joints 1, 9, 10 and 11 and takes joint 1's child.
// Keep that contract when accepting presentation-only archives from a pause-screen pack.
bool pause_screen_valid(const std::vector<uint8_t>& bytes, std::string* error) {
  VisualLayout layout;
  if (!parse_visual_layout(bytes, &layout, error)) return false;
  if (layout.roots.size() != 1 || layout.roots[0] != "ScGamPause_scene_data" || be32(bytes.data() + 16) != 0) {
    *error = "A pause screen must contain only ScGamPause_scene_data, without external code references."; return false;
  }
  disc_metadata::Archive archive(bytes);
  uint32_t scene, models, model, joint;
  if (!archive.parse() || !archive.root("ScGamPause_scene_data", &scene) || !archive.bounds(scene, 16) ||
      !archive.pointer(scene, &models) || !archive.bounds(models, 8) || archive.word(models + 4) ||
      !archive.pointer(models, &model) || !archive.bounds(model, 16) || !archive.pointer(model, &joint)) {
    *error = "The pause screen has no supported scene model."; return false;
  }
  std::set<uint32_t> visited;
  std::vector<uint32_t> order;
  std::function<bool(uint32_t, unsigned)> walk = [&](uint32_t at, unsigned depth) {
    if (depth > 128 || order.size() >= 4096 || !archive.bounds(at, 64) || !visited.insert(at).second) return false;
    order.push_back(at);
    // Particle, spline and instance joints can reach runtime tables outside this visual archive.
    if (archive.word(at + 4) & (0x20u | 0x4000u | 0x1000u)) return false;
    for (uint32_t field : {8u, 12u}) {
      uint32_t next;
      if (archive.pointer(at + field, &next)) { if (!walk(next, depth + 1)) return false; }
      else if (archive.word(at + field)) return false;
    }
    return true;
  };
  uint32_t child;
  if (!walk(joint, 0) || order.size() < 12 || !archive.pointer(order[1] + 8, &child) || !visited.count(child)) {
    *error = "The pause screen is missing required controls joints or has a cyclic joint tree."; return false;
  }
  return true;
}

bool pause_filename(const std::string& name) {
  const std::string base = lower(path_filename_utf8(fs::u8path(name)));
  return base == "gmpause.usd" || base == "gmpause.dat" || base == "gmpausenohud.usd" || base == "gmpausenohud.dat";
}

bool pause_resource(const fs::path& source, const std::string& member, std::vector<uint8_t> bytes,
                    VaultResourcePlan* out, std::string* error) {
  if (!pause_screen_valid(bytes, error)) return false;
  out->source_id = "pause:" + sha256(bytes);
  out->source_member = member;
  out->kind = "pause_screen"; out->target_path = "GmPause.usd";
  out->group = "Menus"; out->slot = "Pause screen";
  const bool no_hud = lower(member.empty() ? path_filename_utf8(source) : member).find("nohud") != std::string::npos;
  out->display_name = source.stem().u8string() + (no_hud ? " (no pause overlay)" : " (PAUSE label)");
  out->bytes = std::move(bytes);
  return true;
}

// The data a costume, stage or effect file gives the game beyond what is drawn, read as the game's
// loaders read it (mod_data_safety.h). A file that fails is refused offline too: the game would stop
// on it, or read past what the file holds. Every retail file passes.
bool costume_structure_safe(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>* standard, std::string* defect) {
  data_safety::Archive archive(bytes);
  if (!archive.parse()) { *defect = "its archive tables do not hold together"; return false; }
  uint32_t joints = 0;
  if (standard) {
    data_safety::Archive original(*standard);
    if (original.parse()) joints = data_safety::costume_joint_count(original);
  }
  return data_safety::costume_structure_valid(archive, joints, defect);
}
// The disc's standard costume of a slot, which the slot's costumes are compared with; null when the
// disc has no such file (a slot a mod adds).
const std::vector<uint8_t>* standard_costume(const std::string& target, std::vector<uint8_t>* storage) {
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(target, &offset, &size) || !size || size > kMaxAssetBytes) return nullptr;
  storage->resize(size);
  return host::disc_read(offset, storage->data(), size) ? storage : nullptr;
}

}  // namespace cosmetic_internal
}  // namespace host::cosmetics
