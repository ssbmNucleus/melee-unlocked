// A small effect file for the slot tests (effect_slots_test.cpp, cosmetic_mods_test.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace effect_slots_fixture {

inline void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - i * 8));
}
inline uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

// A small EfCaData.dat: three model effects (0 and 1 Falcon Punch, 2 Falcon Kick; 1 and 2 share a
// block), four generators (4000 Kick on texture group 0; 4001, 4002 Punch on group 1; 4003 Punch on
// group 0, so group 0 is Shared), `gap` unused bytes after the texture bank (a file laid out anew).
// The offsets are of bytes in the file: a Punch generator's program, group 0's image, model 2's own
// joint, the block models 1 and 2 share, and a block of model 2's alone. `picture`: an HSD image
// descriptor and its 8x8 image after the models, reached by nothing (the catalog's importer wants one).
struct Made { std::vector<uint8_t> bytes; uint32_t gen4001, image0, model2, shared, kick; };
inline Made make(uint32_t gap, const std::function<void(std::vector<uint8_t>&, const Made&)>& edit = nullptr,
                 bool picture = false) {
  const uint32_t C = 0x48, X = 0x164, M = 0x1E8 + gap, size = M + (picture ? 0xC0 : 0x80);
  std::vector<uint8_t> data(size);
  // (the models' bytes by their place from M, so a gap moves them unchanged; the gap is zeros)
  for (uint32_t i = 0; i < size; ++i)
    data[i] = i >= M - gap && i < M ? 0 : uint8_t((i >= M ? i - gap : i) * 7 + 3);
  std::vector<uint32_t> relocs;
  const auto ptr = [&](uint32_t field, uint32_t target) { put32(data, field, target); relocs.push_back(field); };
  // The table: the banks, then three EF_EffectDesc (lifetime, joint, animjoint, matanim, shapeanim).
  ptr(0, C);
  ptr(4, X);
  for (uint32_t i = 0; i < 3; ++i) {
    put32(data, 8 + 0x14 * i, 0x42c80000);   // 100.0
    ptr(8 + 0x14 * i + 4, M + 0x20 * i);
    for (uint32_t k = 2; k < 5; ++k) put32(data, 8 + 0x14 * i + 4 * k, 0);
  }
  put32(data, 0x44, 0);
  // The command bank: version 0x41, first id 4000, four records of 0x40 bytes.
  put32(data, C, 0x00410000);
  put32(data, C + 4, 4000);
  put32(data, C + 8, 4);
  const uint16_t groups[4] = {0, 1, 1, 0};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t rec = 0x1C + 0x40 * i;
    put32(data, C + 12 + 4 * i, rec);
    data[C + rec + 2] = 0;
    data[C + rec + 3] = uint8_t(groups[i]);
  }
  // The texture bank: two groups of one 8x8 I4 image each.
  put32(data, X, 2);
  put32(data, X + 4, 0xC);
  put32(data, X + 8, 0x28);
  for (uint32_t g = 0; g < 2; ++g) {
    const uint32_t head = X + 0xC + 0x1C * g;
    put32(data, head, 1);         // one image
    put32(data, head + 4, 0);     // I4
    put32(data, head + 8, 0);
    put32(data, head + 0xC, 8);
    put32(data, head + 0x10, 8);
    put32(data, head + 0x14, 0);
    put32(data, head + 0x18, 0x44 + 0x20 * g);
  }
  // The models: 0 alone; 1 points at the shared block; 2 at the shared block and its own.
  ptr(M + 0x30, M + 0x60);
  ptr(M + 0x50, M + 0x60);
  ptr(M + 0x54, M + 0x70);
  if (picture) {
    // (image, width and height, I4, no mipmaps, LODs 0)
    ptr(M + 0x80, M + 0xA0);
    put32(data, M + 0x84, 8u << 16 | 8u);
    for (uint32_t k = 8; k < 0x18; k += 4) put32(data, M + 0x80 + k, 0);
  }
  Made made{{}, 0x20 + C + 0x1C + 0x40 + 0x20, 0x20 + X + 0x44 + 5, 0x20 + M + 0x48, 0x20 + M + 0x64,
            0x20 + M + 0x78};
  // The archive.
  const std::string name = "effCaptainDataTable";
  std::vector<uint8_t>& b = made.bytes;
  b.resize(0x20 + size + 4 * relocs.size() + 8 + name.size() + 1);
  put32(b, 4, size);
  put32(b, 8, uint32_t(relocs.size()));
  put32(b, 12, 1);
  std::memcpy(b.data() + 0x20, data.data(), size);
  for (size_t i = 0; i < relocs.size(); ++i) put32(b, 0x20 + size + 4 * i, relocs[i]);
  put32(b, 0x20 + size + 4 * relocs.size(), 0);
  put32(b, 0x20 + size + 4 * relocs.size() + 4, 0);
  std::memcpy(b.data() + 0x20 + size + 4 * relocs.size() + 8, name.c_str(), name.size() + 1);
  put32(b, 0, uint32_t(b.size()));
  if (edit) edit(b, made);
  return made;
}

}  // namespace effect_slots_fixture
