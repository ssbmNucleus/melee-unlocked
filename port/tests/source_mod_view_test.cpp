// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_online_policy.h"
#include <cstdio>
#include <initializer_list>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
  using namespace slippi::online;
  const std::string fp = "0123456789abcdef" + std::string(48, 'a');
  struct FingerprintCase { std::string value; bool valid; };
  const FingerprintCase fingerprints[] = {
    {fp, true}, {"", false}, {fp.substr(0, 63), false}, {fp + "0", false},
    {std::string(64, 'g'), false}, {std::string(64, 'A'), false}
  };

  // Full cross product, including every mode, two offline IDs and an unknown online ID.
  // Today's standalone table: no mod -> retail; offline -> mod; Direct -> mod iff
  // enabled and not a plain opponent; Ranked/Unranked/Teams/Party/unknown -> retail.
  int rows = 0;
  for (bool has_mod : {false, true})
    for (int mode : {-2, -1, 0, 1, 2, 3, 4, 5})
      for (bool plain : {false, true})
        for (bool setting : {false, true})
          for (const auto& fingerprint : fingerprints) {
            CHECK(native_mod_fingerprint_valid(fingerprint.value) == fingerprint.valid);
            CHECK(native_source_mod_view(has_mod, mode, plain, setting) ==
                  (has_mod && (mode < 0 || (mode == 2 && !plain && setting))));
            CHECK(native_source_mod_view(has_mod, mode, plain, setting, true) ==
                  (has_mod && mode < 0));
            ++rows;
          }
  std::printf("source_mod_view: %d decision rows passed\n", rows);
}
