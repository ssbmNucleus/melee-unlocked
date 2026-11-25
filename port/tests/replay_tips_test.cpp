// The replay viewer's last / next tip (replay_tips.h).
#include "replay_tips.h"

#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  using replay_tips::pick;
  const std::vector<int32_t> s{600, 1500, 1520, 4000};
  // Y: the next tip after where the viewer is.
  CHECK(pick(s, 0, false) == 0);
  CHECK(pick(s, 600, false) == 1);
  CHECK(pick(s, 1500, false) == 2);
  CHECK(pick(s, 4000, false) == -1);
  // X right after a jump (playhead on the tip's start, or a few frames into it): the tip before.
  CHECK(pick(s, 1500, true) == 0);
  CHECK(pick(s, 1500 + 30, true) == 1);            // 1530 is past 1520's start: 1520 is current, X goes to 1500
  CHECK(pick(s, 4000 + 100, true) == 2);           // watching the last tip: X goes to the one before
  // X long after a tip: back to that tip's start.
  CHECK(pick(s, 3000, true) == 2);
  CHECK(pick(s, 600 + 500, true) == 0);
  // X at the first tip, or before any: nothing to go back to.
  CHECK(pick(s, 610, true) == -1);
  CHECK(pick(s, 100, true) == -1);
  // Pressing X again and again from where each jump lands walks back one tip at a time, never stays.
  int at = 4000, steps = 0;
  for (int i = pick(s, at, true); i >= 0; i = pick(s, at, true)) { CHECK(s[i] < at); at = s[i]; ++steps; }
  CHECK(steps == 3 && at == 600);
  // And Y from each landing walks forward one at a time.
  at = 0; steps = 0;
  for (int i = pick(s, at, false); i >= 0; i = pick(s, at, false)) { CHECK(s[i] > at); at = s[i]; ++steps; }
  CHECK(steps == 4 && at == 4000);
  CHECK(pick({}, 500, true) == -1 && pick({}, 500, false) == -1);
  std::printf(failures ? "replay_tips: %d failed\n" : "replay_tips: ok\n", failures);
  return failures ? 1 : 0;
}
