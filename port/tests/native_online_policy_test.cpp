// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_online_policy.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
  using slippi::online::NativeGameplayProfile;
  using slippi::online::native_profile_mode_error;
  for (int mode = 0; mode <= 4; ++mode) {
    CHECK(native_profile_mode_error(NativeGameplayProfile::Vanilla, mode) == nullptr);
    CHECK((native_profile_mode_error(NativeGameplayProfile::Akaneia, mode) == nullptr) == (mode == 2 || mode == 3));
    // Every gameplay mod is Direct and Teams only (against the same build, checked in netplay).
    CHECK((native_profile_mode_error(NativeGameplayProfile::OtherMod, mode) == nullptr) == (mode == 2 || mode == 3));
  }

  CHECK(native_profile_mode_error(NativeGameplayProfile::Vanilla, -1) != nullptr);
  CHECK(native_profile_mode_error(NativeGameplayProfile::Vanilla, 5) != nullptr);
  for (auto mod : {NativeGameplayProfile::Akaneia, NativeGameplayProfile::OtherMod}) {
    const auto* reason = native_profile_mode_error(mod, 0);
    CHECK(reason && std::strstr(reason, "Ranked"));
  }
  std::puts("native_online_policy: ok");
}
