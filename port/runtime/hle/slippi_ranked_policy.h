#pragma once
#include <cstdint>
namespace slippi::online {
inline const char* ranked_status(uint8_t index) {
  constexpr const char* setup[] = {"game_setup_1","game_setup_2","game_setup_3","game_setup_4","game_setup_5","game_setup_6","game_setup_7"};
  constexpr const char* start[] = {"game_start_1","game_start_2","game_start_3","game_start_4","game_start_5","game_start_6","game_start_7"};
  if (index >= 10 && index <= 16) return setup[index - 10];
  if (index >= 20 && index <= 26) return start[index - 20];
  switch (index) {
    case 1: return "connecting";
    case 30: return "normal_completion";
    case 31: return "abnormal_completion";
    case 40: return "abandoned";
    default: return nullptr;
  }
}
}
