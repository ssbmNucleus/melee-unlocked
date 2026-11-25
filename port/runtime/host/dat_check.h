// The structure of a DAT archive (HSD): what the game's loader relies on when it relocates a file in
// place (sysdolphin/baselib/archive.c), checked on this side before a file is used. One place for
// it: cosmetic imports and the fighters folder (roster.cpp) both read it.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host::dat {

enum class Rules {
  // What the game's own loader takes; every file of the retail disc passes. TyMnInfo.dat has
  // relocation words off a 4-byte boundary, and SdClr.usd, SdIntro.dat, SdMsgBox.usd and
  // SdProge.usd pointers just past their data block.
  Loader,
  // Cosmetic imports (a costume), stricter: relocation words on a 4-byte boundary pointing inside
  // the data block, 1 to 1024 roots and at most 1024 references, printable root symbols of at most
  // 255 characters.
  Cosmetic,
};

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// Empty when `bytes` is a whole archive by `rules`, else why it is not. The header against the
// file's length; the relocation, root and reference tables inside the file; each relocation a word
// of the data block (no word named twice) holding an offset into it; each root and reference inside
// the data block with its symbol terminated inside the string table. `roots`, when given, receives
// the root symbols in order.
inline std::string structure_error(const uint8_t* bytes, size_t size, Rules rules,
                                   std::vector<std::string>* roots = nullptr) {
  const bool cosmetic = rules == Rules::Cosmetic;
  if (size < 0x20) return "DAT header is truncated.";
  const uint64_t file_size = be32(bytes), data_size = be32(bytes + 4), relocations = be32(bytes + 8),
                 publics = be32(bytes + 12), references = be32(bytes + 16);
  if (file_size != size) return "DAT header size does not match the file length.";
  if (relocations > size / 4 || (cosmetic && (!publics || publics > 1024 || references > 1024)))
    return "DAT relocation/root counts are outside supported bounds.";
  const uint64_t root_table = 0x20ull + data_size + relocations * 4ull;
  const uint64_t strings = root_table + (publics + references) * 8ull;
  if (root_table > size || strings > size) return "DAT tables point outside the file.";
  // Every relocation identifies a pointer field in the data block. The pointer itself is either null
  // or another data-block-relative offset. Real Melee DAT relocation lists are not necessarily
  // sorted, so ordering is deliberately not part of the contract; a word relocated twice is.
  std::vector<uint32_t> words;
  words.reserve((size_t)relocations);
  for (uint64_t i = 0; i < relocations; ++i) {
    const uint64_t relocation_offset = be32(bytes + 0x20 + data_size + i * 4);
    if ((cosmetic && (relocation_offset & 3)) || relocation_offset + 4 > data_size)
      return "DAT relocation entry is unaligned or points outside the data block.";
    const uint64_t target = be32(bytes + 0x20 + relocation_offset);
    if (cosmetic ? (target && target >= data_size) : target > data_size)
      return "DAT relocation target points outside the data block.";
    words.push_back((uint32_t)relocation_offset);
  }
  std::sort(words.begin(), words.end());
  for (size_t i = 1; i < words.size(); ++i)
    if ((uint64_t)words[i] < (uint64_t)words[i - 1] + 4) return "DAT relocation entries overlap.";
  for (uint64_t i = 0; i < publics + references; ++i) {
    const uint64_t record = root_table + i * 8;
    const uint64_t object_offset = be32(bytes + record), name_offset = be32(bytes + record + 4);
    if (object_offset >= data_size || name_offset >= size - strings)
      return "DAT root entry points outside its data/string table.";
    const size_t begin = (size_t)(strings + name_offset);
    size_t end = begin;
    while (end < size && bytes[end]) {
      if (cosmetic && (bytes[end] < 0x20 || bytes[end] > 0x7e || end - begin > 255))
        return "DAT root symbol is not a bounded printable string.";
      ++end;
    }
    if (end == size) return "DAT root symbol is not terminated.";
    if (roots && i < publics) roots->emplace_back((const char*)bytes + begin, end - begin);
  }
  return {};
}

// A fighter's animation file (PlXxAJ.dat) is not one archive: it is the archive of each animation,
// one after another, each starting on a 32-byte boundary (the fighter's data file gives the game the
// offset and size of each). Zero bytes may close it. Empty when every one is a whole archive.
inline std::string animation_file_error(const uint8_t* bytes, size_t size) {
  size_t at = 0;
  int count = 0;
  while (at < size) {
    if (std::all_of(bytes + at, bytes + size, [](uint8_t b) { return b == 0; })) break;
    if (size - at < 0x20) return "an animation's archive is truncated";
    const uint64_t length = be32(bytes + at);
    if (length < 0x20 || length > size - at) return "an animation's archive does not fit the file";
    const std::string error = structure_error(bytes + at, (size_t)length, Rules::Loader);
    if (!error.empty()) return "animation " + std::to_string(count) + ": " + error;
    ++count;
    at = (size_t)((at + length + 31) & ~(uint64_t)31);
  }
  return count ? std::string() : "it holds no animation";
}

}  // namespace host::dat
