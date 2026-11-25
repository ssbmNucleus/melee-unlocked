// Exercise the matchmaking library shipped with Slippi Dolphin and the standalone client
// against the same local service. Run with tools/check_slippi_ranked_queue.py on macOS.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "slippi_mm_service.h"

#include <nlohmann/json.hpp>
#include <dlfcn.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace host {
void log(const char*, ...) {}
}

namespace {
// C ABI of project-slippi/slippi-rust-extensions, ffi/src/{exi,matchmaking}.rs at 3.7.0.
struct DeviceConfig {
  const char *iso, *user_directory, *version;
  void (*on_message)(const char*, uint32_t, uint32_t);
};
struct Observation {
  bool answered;
  uint8_t address[4];
  uint16_t port;
};
struct MatchRequest {
  uint8_t mode;
  const uint8_t* connect_code;
  size_t connect_code_length;
  uint16_t port;
  const char* lan_address;
  Observation observations[2];
};

template <typename Function> Function resolve(void* library, const char* name) {
  auto address = dlsym(library, name);
  if (!address) throw std::runtime_error(name);
  return reinterpret_cast<Function>(address);
}
}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::cerr << dlerror() << '\n'; return 1; }
  const auto create = resolve<size_t(*)(DeviceConfig)>(library, "slprs_exi_device_create");
  const auto start = resolve<uint64_t(*)(size_t, MatchRequest)>(library, "slprs_mm_start");
  const auto state = resolve<int(*)(size_t)>(library, "slprs_mm_state");
  const auto take = resolve<char*(*)(size_t)>(library, "slprs_mm_take_result_json");
  const auto release = resolve<void(*)(char*)>(library, "slprs_mm_free_string");
  const auto destroy = resolve<void(*)(size_t)>(library, "slprs_exi_device_destroy");
  setenv("SLIPPI_MM_URL", argv[3], 1);
  const size_t device = create({"", argv[2], "3.7.0", [](const char*, uint32_t, uint32_t) {}});
  MatchRequest stock{};
  stock.mode = 0;
  stock.port = 49122;
  stock.lan_address = "127.0.0.1:49122";
  start(device, stock);

  namespace service = slippi::mm_service;
  service::Request request{};
  request.mode = 0;
  request.netplay_port = 49121;
  request.lan_addr = "127.0.0.1:49121";
  request.app_version = "3.7.0";
  request.uid = "test-fork";
  request.play_key = "loopback-only";
  request.display_name = "Fork";
  request.connect_code_self = "TEST#1";
  service::Session fork(request, argv[3]);
  using namespace std::chrono;
  const auto deadline = steady_clock::now() + seconds(10);
  while (steady_clock::now() < deadline &&
         (state(device) < 3 || fork.state() < service::State::Matched)) {
    std::this_thread::sleep_for(milliseconds(50));
  }
  const char* expected = "mode.ranked-loopback";
  char* raw = take(device);
  std::string other;
  bool passed = false;
  if (raw && fork.take_result_json(&other)) {
    const auto a = nlohmann::json::parse(raw);
    const auto b = nlohmann::json::parse(other);
    passed = a.at("matchId") == expected && b.at("matchId") == expected &&
             a.at("players").size() == 2 && b.at("players").size() == 2 &&
             a.at("isHost") != b.at("isHost");
  }
  if (raw) release(raw);
  destroy(device);
  std::cout << (passed ? "PASS" : "FAIL") << " Slippi 3.7 and fork Ranked queue assignment\n";
  return passed ? 0 : 1;
}
