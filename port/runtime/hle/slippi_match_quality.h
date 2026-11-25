// SlippiRustExtensions/match-quality: Ranked match termination criteria.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace slippi {
class MatchQuality {
public:
  bool should_terminate(int frame, int latest_remote, int delay, bool stalled, uint64_t now_us) {
    if (terminated_) return false;
    depth_sum_ += std::clamp(frame - latest_remote - std::max(0, 2 - delay), 0, 8);
    ++samples_;
    if (stalled) {
      if (frame > 120) {
        ++stalled_passes_;
        if (!last_stall_us_ || now_us - last_stall_us_ > 500000) stall_length_ = 0;
        last_stall_us_ = now_us;
        if (++stall_length_ == 5) ++stall_events_;
      }
      return false;
    }
    if (frame < 120 || (frame - 120) % 150 || frame == last_boundary_) return false;
    if (last_boundary_) {
      const double overrun = (now_us - start_us_) / (16683.0 * 150) - 1;
      const double depth = double(depth_sum_) / samples_;
      const double scale = frame > 3600 ? 1.15 : 1.0;
      const int stalls = stall_events_ ? 9 : stalled_passes_ ? 3 : -1;
      const int change = std::max({tier(overrun / scale, .1, .5, .75),
                                   tier(depth / scale, 2, 3, 4.75), stalls});
      debt_ = std::max(0, debt_ + change);
    }
    last_boundary_ = frame;
    start_us_ = now_us;
    samples_ = depth_sum_ = stalled_passes_ = stall_events_ = 0;
    terminated_ = debt_ >= 30;
    return terminated_;
  }

private:
  static int tier(double value, double mild, double bad, double severe) {
    if (value >= severe) return 15;
    if (value >= bad) return 8;
    return value >= mild ? 4 : -1;
  }
  int last_boundary_ = 0, debt_ = 0;
  uint64_t depth_sum_ = 0, samples_ = 0, start_us_ = 0, last_stall_us_ = 0;
  unsigned stall_length_ = 0, stalled_passes_ = 0, stall_events_ = 0;
  bool terminated_ = false;
};
}
