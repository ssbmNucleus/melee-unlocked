// Offline timing probe for the production sampler and its transport callback.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "early_input.h"
#include "host.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int kFrames = 120;
constexpr int kFirstFrame = 100;
constexpr uint16_t kButtonA = 0x0100;
constexpr uint16_t kButtonB = 0x0200;
std::atomic<uint16_t> buttons{kButtonA};
std::vector<Clock::time_point> reads;

struct Send {
  int frame;
  uint16_t buttons;
  Clock::time_point when;
};
std::vector<Send> sends;

void capture(int32_t frame, const uint8_t pad[host::early_input::kPadBytes]) {
  sends.push_back({frame, uint16_t((pad[0] << 8) | pad[1]), Clock::now()});
}
double milliseconds(Clock::duration elapsed) {
  return std::chrono::duration<double, std::milli>(elapsed).count();
}
}

namespace host {
void log(const char*, ...) {}
double now_seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
uint32_t retrace_count() { return 0; }
bool input_script_active() { return false; }
void input_script_peek(uint32_t, PadState[4]) {}
TickTiming& tick_timing() { static TickTiming timing; return timing; }

void input_last_pads(PadState pads[4]) {
  for (int port = 0; port < 4; ++port) {
    pads[port] = {};
    pads[port].err = port == 0 ? 0 : -1;
  }
  pads[0].button = buttons.load();
}
void input_read_devices(PadState pads[4]) {
  reads.push_back(Clock::now());
  input_last_pads(pads);
}
}

int main() {
  namespace early = host::early_input;
  const bool initial = early::enabled();
  reads.reserve(3000);
  sends.reserve(kFrames);
  std::vector<Clock::time_point> next_polls(kFrames);
  early::set_enabled(true);
  early::start(capture);

  uint8_t wire[early::kPadBytes] = {uint8_t(kButtonA >> 8)};
  for (int frame = 0; frame < kFrames; ++frame) {
    early::note_sent_pad(kFirstFrame + frame, wire);
    const auto start = Clock::now();
    std::this_thread::sleep_until(start + 6ms);
    const uint16_t next = frame % 2 ? kButtonA : kButtonB;
    buttons.store(next);
    wire[0] = uint8_t(next >> 8);
    std::this_thread::sleep_until(start + 16667us);
    next_polls[frame] = Clock::now();
  }
  early::stop();

  std::vector<double> intervals, leads;
  for (size_t i = 1; i < reads.size(); ++i)
    intervals.push_back(milliseconds(reads[i] - reads[i - 1]));
  std::vector<bool> seen(kFrames);
  for (const auto& send : sends) {
    const int frame = send.frame - kFirstFrame - 1;
    if (frame < 0 || frame >= kFrames || send.buttons != (frame % 2 ? kButtonA : kButtonB))
      return 1;
    if (!seen[frame]) {
      const double lead = milliseconds(next_polls[frame] - send.when);
      if (lead > 0) leads.push_back(lead);
      seen[frame] = true;
    }
  }
  if (intervals.empty() || leads.empty()) return 1;
  std::sort(intervals.begin(), intervals.end());
  std::sort(leads.begin(), leads.end());
  std::printf("Initial setting %s. %zu reads, median interval %.3f ms, p95 %.3f ms. "
              "%zu of %d changes reached the callback before the next poll, median lead %.3f ms.\n",
              initial ? "on" : "off", reads.size(), intervals[intervals.size() / 2],
              intervals[intervals.size() * 95 / 100], leads.size(), kFrames, leads[leads.size() / 2]);
}
