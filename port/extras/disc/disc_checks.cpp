// Disc image checks from the old Windows launcher (port/app/launcher.cpp), kept for reuse by an
// external launcher. Not built by this repository. Build on Windows with C++17 and link bcrypt.
#include "disc_checks.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace extras::disc {

bool verify_iso(const std::string& path, std::string* why) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) { *why = "cannot open the file"; return false; }
  char hdr[8]{}; f.read(hdr, 8);
  if (std::string(hdr, 6) != "GALE01") { *why = "not a Melee NTSC disc (game id " + std::string(hdr, 6) + ")"; return false; }
  if (hdr[7] != 2) { *why = "Melee NTSC 1.0" + std::to_string((int)hdr[7]) + "; version 1.02 is required"; return false; }
  f.seekg(0, std::ios::end);
  auto size = (unsigned long long)f.tellg();
  if (size < 1000000000ull) { *why = "file is too small to be a full disc image (" + std::to_string(size / 1000000) + " MB)"; return false; }
  return true;
}

// An m-ex mod disc: its file table names MxDt.dat. `f` is the open disc image.
static bool mex_disc(std::ifstream& f) {
  unsigned char at[8]{};
  f.clear(); f.seekg(0x424);
  if (!f.read((char*)at, 8)) return false;
  const uint32_t offset = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
  const uint32_t size = ((uint32_t)at[4] << 24) | ((uint32_t)at[5] << 16) | ((uint32_t)at[6] << 8) | at[7];
  if (size < 12 || size > (16u << 20)) return false;
  std::vector<char> fst(size);
  f.seekg(offset);
  if (!f.read(fst.data(), size)) return false;
  const uint32_t entries = ((uint32_t)(uint8_t)fst[8] << 24) | ((uint32_t)(uint8_t)fst[9] << 16) | ((uint32_t)(uint8_t)fst[10] << 8) | (uint8_t)fst[11];
  if (!entries || (uint64_t)entries * 12 >= size) return false;
  for (size_t i = (size_t)entries * 12; i + 8 <= size; ++i)   // the names, NUL-separated
    if ((i == (size_t)entries * 12 || fst[i - 1] == 0) && _strnicmp(fst.data() + i, "MxDt.dat", 8) == 0 && (i + 8 == size || fst[i + 8] == 0))
      return true;
  return false;
}

static const wchar_t* const kMexDiscProblem =
    L"This is a mod disc (Akaneia, ACE or another m-ex build). It plays with its own content as a mod. "
    L"The base disc has to be your original Melee NTSC 1.02 ISO.";

std::wstring base_disc_problem(const std::string& path) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) return L"The disc image could not be opened. Choose your Melee NTSC 1.02 ISO again.";
  unsigned char hdr[8]{}; f.read((char*)hdr, 8);
  if (!std::memcmp(hdr, "RVZ\x01", 4) || !std::memcmp(hdr, "WIA\x01", 4) || !std::memcmp(hdr, "CISO", 4) ||
      (hdr[0] == 0x01 && hdr[1] == 0xC0 && hdr[2] == 0x0B && hdr[3] == 0xB1))
    return L"This disc image is compressed (RVZ, GCZ, WIA or CISO). In Dolphin, right-click the game, choose Convert File and pick ISO, then choose that ISO here.";
  if (std::memcmp(hdr, "GALE01", 6) != 0)
    return L"This file is not a Melee NTSC disc image. Choose your own Melee NTSC 1.02 ISO.";
  if (hdr[7] != 2)
    return L"This is Melee NTSC 1.0" + std::to_wstring((int)hdr[7]) + L". Version 1.02 is required.";
  // A mod disc keeps the game id. Its game code (main.dol) differs from the original, which both
  // engines check at startup (host.cpp disc_has_vanilla_dol: the same size and SHA-1).
  unsigned char at[4]{};
  f.seekg(0x420); f.read((char*)at, 4);
  const uint32_t dol = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
  std::vector<unsigned char> image(0x4385E0u);
  f.seekg(dol); f.read((char*)image.data(), (std::streamsize)image.size());
  if (!f) return L"This file is not a full Melee disc image. Choose a clean, uncompressed Melee NTSC 1.02 ISO.";
  const unsigned char expected[20] = {0x08,0xe0,0xbf,0x20,0x13,0x4d,0xfc,0xb2,0x60,0x69,0x96,0x71,0x00,0x45,0x27,0xb2,0xd6,0xbb,0x1a,0x45};
  unsigned char digest[20]{};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) return {};
  const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, image.data(), (ULONG)image.size(), digest, sizeof digest);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status >= 0 && std::memcmp(digest, expected, sizeof digest) != 0)
    return L"This is a modded Melee disc. The base disc has to be your original Melee NTSC 1.02 ISO.";
  // m-ex builds (Akaneia, ACE) keep the original game code and bring their own in files, so the
  // check above passes them; started as the base disc they play as plain Melee. m-ex's own table,
  // MxDt.dat, is in the disc's file table.
  if (mex_disc(f)) return kMexDiscProblem;
  return {};
}

bool nkit_image(const std::wstring& path) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  char tag[4] = {};
  if (!in.seekg(0x200) || !in.read(tag, 4)) return false;
  return std::memcmp(tag, "NKIT", 4) == 0;
}

}  // namespace extras::disc
