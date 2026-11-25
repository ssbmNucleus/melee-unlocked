// Test the actual Slippi game-report worker against a local service using a synthetic account.
// Include the implementation to supply a test endpoint without a configurable production endpoint.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_report.cpp"
#include <cstdarg>
namespace host {
void log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  std::fputc('\n', stderr);
  va_end(args);
}
}

int main(int argc, char** argv) {
  if (argc != 4 || std::string(argv[1]).find("http://127.0.0.1:") != 0) return 2;
  using namespace slippi::report;
  ENDPOINT = argv[1];
  init(argv[2], argv[3]);
  for (int game = 1; game <= 2; ++game) {
    GameReport report;
    report.match_id = "mode.ranked-loopback";
    report.uid = "test-account";
    report.play_key = "test-key";
    report.game_index = game;
    report.players.resize(4);
    report.replay = start_replay();
    const uint8_t table[] = {0x35, 7, 0x38, 4, 0, 0x39, 0, 2};
    append_replay(*report.replay, table, sizeof table);
    uint8_t frame[1025] = {0x38};
    append_replay(*report.replay, frame, sizeof frame);
    const uint8_t end[] = {0x39, 2, uint8_t(game)};
    log_game(report);
    append_replay(*report.replay, end, sizeof end);
  }
  match_status("test-account", "test-key", "mode.ranked-loopback", "normal_completion", true);
  fetch_match_result("mode.ranked-loopback", "test-account", "test-key");
  RankInfo result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (rank_info(&result) == RankFetchStatus::Fetching && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  bool received = rank_info(&result) == RankFetchStatus::Fetched && result.rank_change == 1 &&
                  result.rating_ordinal == 2005 && result.rating_change == 10 && result.rating_update_count == 8;
  shutdown();
  return received ? 0 : 1;
}
