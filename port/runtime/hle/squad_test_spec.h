// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#pragma once
#if MU_SQUAD
#include "../abi/mu_squad_wire.h"
namespace slippi::squad_test {
// Decimal only, no signs, whitespace, empty fields or trailing separators.
inline bool parse(const char* text, MuSquadSide* out) {
  if (!text || !out) return false;
  MuSquadSide value{};
  const char* p = text;
  auto number = [&p](unsigned& n) {
    n = 0;
    if (*p < '0' || *p > '9') return false;
    do { n = n * 10 + unsigned(*p++ - '0'); if (n > 255) return false; }
    while (*p >= '0' && *p <= '9');
    return true;
  };
  for (;;) {
    unsigned kind, color;
    if (value.count == MU_SQUAD_LINEUP || !number(kind) || *p++ != '/' || !number(color) ||
        kind > 25 || color > 5) return false;
    value.members[value.count++] = {(unsigned char)kind, (unsigned char)color};
    if (!*p) break;
    if (*p++ != ',') return false;
  }
  // Apply the same per-fighter costume range as the production lineup command.
  if (!mu_squad_side_valid(&value)) return false;
  *out = value;
  return true;
}

// MELEE_TEST_ONLINE_SQUAD (Squad Strike in Direct, local peering only): "<P1>;<P2>", each side 1 to 4
// "kind/color" in fighting order, comma-separated, as mu_squad_lineup_valid allows.
inline bool parse_direct(const char* text, MuSquadSide* p1, MuSquadSide* p2) {
  if (!text || !p1 || !p2) return false;
  MuSquadSide sides[2]{};
  const char* p = text;
  for (int s = 0; s < 2; ++s) {
    for (;;) {
      unsigned kind = 0, color = 0;
      if (sides[s].count == MU_SQUAD_LINEUP || *p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') { kind = kind * 10 + unsigned(*p++ - '0'); if (kind > 255) return false; }
      if (*p++ != '/' || *p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') { color = color * 10 + unsigned(*p++ - '0'); if (color > 255) return false; }
      if (kind > 25) return false;
      sides[s].members[sides[s].count++] = {(unsigned char)kind, (unsigned char)color};
      if (*p == ',') { ++p; continue; }
      break;
    }
    if (s == 0 && *p++ != ';') return false;
    if (!mu_squad_lineup_valid(&sides[s])) return false;
  }
  if (*p) return false;
  *p1 = sides[0];
  *p2 = sides[1];
  return true;
}
} // namespace slippi::squad_test
#endif
