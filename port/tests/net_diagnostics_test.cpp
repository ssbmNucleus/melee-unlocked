#include "netplay_summary.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
int main() {
  slippi::diagnostics::Summary s;
  s.begin(2); s.frames = 120; s.max_rollback = 7; s.held_up = 2; s.fell_behind = 3;
  for (unsigned i = 1; i <= 100; ++i) s.ping.add(i);
  CHECK(s.line(9, 4, 1, "completed") == "netplay summary: mode direct frames 120 secs 2.000 ping p50 50 p95 95 max 100 rollbacks 9 max-rollback 7 stalls 4 held-up 2 fell-behind 3 desyncs 1 result completed");
  s.begin(1); CHECK(s.ping.count == 0 && s.mode == "unranked" && s.held_up == 0);
  s.begin(0);
  CHECK(s.mode == "ranked");
  return 0;
}
