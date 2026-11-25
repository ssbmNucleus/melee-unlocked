// Picking the program for this processor, from the old Windows launcher (port/app/launcher.cpp).
// Not built by this repository; see ../README.md.
//
// The ordinary build needs AVX2 (Intel Haswell, 2013, or AMD Ryzen and later). On a processor
// without it melee_source.exe stops at startup with exit code 3 and a message (with --no-dialogs
// on stderr only); melee_source --print-info answers cpu_supported false without starting. A
// release may carry a second build for those
// processors, melee_source_compat.exe with melee_game_compat.dll beside it; a launcher picks
// between the two by asking the processor, as below.
#pragma once
#include <intrin.h>
#include <string>
#include <sys/stat.h>

namespace extras::launch {

inline bool cpu_has_avx2() {
  int r[4];
  __cpuid(r, 0); if (r[0] < 7) return false;
  __cpuid(r, 1); const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
  if (!osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;
  __cpuidex(r, 7, 0); return (r[1] >> 5) & 1;
}

// The Source Port program in `dir` for this processor. `force_compat` is a player's own override.
inline std::string source_exe_in(const std::string& dir, bool force_compat = false) {
  struct _stat64 st;
  const std::string compat = dir + "\\melee_source_compat.exe";
  if ((force_compat || !cpu_has_avx2()) && _stat64(compat.c_str(), &st) == 0) return compat;
  return dir + "\\melee_source.exe";
}

}  // namespace extras::launch
