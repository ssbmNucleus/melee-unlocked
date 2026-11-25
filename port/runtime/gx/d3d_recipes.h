// The D3D11 and D3D12 renderers share the same GPU-independent recipe format.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "gx_regs.h"
#include "gx_texture.h"

namespace gx {
struct PipelineRecipe {
  uint32_t topology = 0, components = 0;
  BPMemory bp{};
  uint32_t xf[0x58]{};
};
static_assert(sizeof(PipelineRecipe) == 1384, "D3D recipe file layout");

inline bool read_recipe_file(const std::filesystem::path& path, std::vector<PipelineRecipe>& out) {
  std::ifstream file(path, std::ios::binary);
  uint64_t header[3]{};
  if (!file.read(reinterpret_cast<char*>(header), sizeof header) ||
      header[0] != 0x3150535247505847ull || header[1] > 16384) return false;
  std::vector<PipelineRecipe> recipes(static_cast<size_t>(header[1]));
  const size_t bytes = recipes.size() * sizeof(PipelineRecipe);
  if (!file.read(reinterpret_cast<char*>(recipes.data()), bytes) ||
      hash_bytes(recipes.data(), bytes) != header[2]) return false;
  out.insert(out.end(), recipes.begin(), recipes.end());
  return true;
}

#ifdef _WIN32
// A double-click launch can put writable shaders in LocalAppData, while the
// versioned starter set always belongs to the installed game. Never write to it.
inline void read_shipped_d3d_recipes(std::vector<PipelineRecipe>& out) {
  std::wstring exe(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  if (!size || size >= exe.size()) return;
  exe.resize(size);
  read_recipe_file(std::filesystem::path(exe).parent_path() / L"shadercache" / L"recipes.shipped.bin", out);
}
#endif
}  // namespace gx
