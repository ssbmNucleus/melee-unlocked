#pragma once
// Which tip X (last) and Y (next) jump to in the replay viewer (replay_hud.cpp).
//
// `starts` are the frames a tip's jump lands on (a second before the tip), ascending. `ref` is where
// the viewer is: the playhead, or while a jump is still on its way, where that jump lands. Using the
// playhead mid-jump read the frame the jump restored from (earlier than its target), so each X picked
// a tip further back and queued another jump: spamming X chained jumps until the game stood still,
// and X after a jump picked the same tip again once playback ran 5 frames into it (tyler631, 11-23).
#include <cstdint>
#include <vector>

namespace replay_tips {

constexpr int32_t kSlack = 5;             // a jump lands within a few frames of its target
constexpr int32_t kRestartWindow = 240;   // within 4 s of a tip's start, X goes to the one before it

// Index into `starts`, or -1 for no jump.
inline int pick(const std::vector<int32_t>& starts, int32_t ref, bool back) {
  if (!back) {
    for (size_t i = 0; i < starts.size(); ++i)
      if (starts[i] > ref + kSlack) return (int)i;
    return -1;
  }
  int current = -1;   // the tip being watched: the last one started by now
  for (size_t i = 0; i < starts.size(); ++i)
    if (starts[i] <= ref + kSlack) current = (int)i;
  if (current < 0) return -1;
  // Just jumped to it, or watching its start: the one before. Long past it: back to its start.
  return ref - starts[current] <= kRestartWindow ? current - 1 : current;
}

}  // namespace replay_tips
