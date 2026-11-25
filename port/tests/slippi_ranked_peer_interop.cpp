// Two peers on loopback. Build with slippi_ranked_peer_interop.py.
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef STOCK_SERIALIZER
#include <SFML/Network/Packet.hpp>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
#include "slippi_stock.inc"

std::vector<uint8_t> stock_packet(int message, uint8_t seat, int health_adjustment) {
  SlippiNetplayClient stock;
  stock.playerIdx = seat;
  if (message == NP_MSG_SLIPPI_COMPLETE_STEP) {
    SlippiGamePrepStepResults step{4, 12, 3, {5, 2}};
    stock.SendGamePrepStep(step);
  } else {
    assert(message == NP_MSG_SLIPPI_SYNCED_STATE);
    SlippiSyncedGameState state;
    state.match_id = "loopback-ranked";
    state.game_index = 2;
    state.tiebreak_index = 1;
    state.seconds_remaining = 321;
    for (int i = 0; i < 4; ++i) {
      state.fighters[i].stocks_remaining = 4 - i;
      state.fighters[i].current_health = 256 + i;
    }
    state.fighters[0].current_health += health_adjustment;
    stock.SendSyncedGameState(state);
  }
  const auto* data = static_cast<const uint8_t*>(stock.out->getData());
  return {data, data + stock.out->getDataSize()};
}

#else
#include "slippi_net.h"
#include "slippi_online.h"
#include <chrono>
#include <cstdio>
#include <enet/enet.h>

// Host presentation/config hooks outside the peer being tested.
namespace gx { uint32_t shield_color_online() { return 0; } }
namespace lcancel { bool automatic_active_online() { return false; } }
namespace host { void log(const char*, ...) {} }
namespace host::early_input { void note_received() {} }
namespace slippi::online {
const LocalBuild& local_build() { static LocalBuild build; return build; }
bool early_inputs_accept() { return false; }
Config& config() { static Config config; return config; }
}
std::vector<uint8_t> stock_packet(int message, uint8_t seat, int health_adjustment = 0);

int main() {
  using namespace slippi;
  assert(enet_initialize() == 0);
  ENetAddress address{};
  assert(enet_address_set_host(&address, "127.0.0.1") == 0);
  auto server = std::unique_ptr<ENetHost, decltype(&enet_host_destroy)>(
      enet_host_create(&address, 1, 3, 0, 0), enet_host_destroy);
  assert(server && server->address.port);
  {
    NetplayClient client({"127.0.0.1"}, {server->address.port}, 1, 0, true, 0);
    GamePrepStep step{4, 12, 3, 5, 2};
    SyncedGameState state;
    state.match_id = "loopback-ranked";
    state.game_index = 2;
    state.tiebreak_index = 1;
    state.seconds_remaining = 321;
    for (int i = 0; i < 4; ++i) state.fighters[i] = {uint8_t(4 - i), uint16_t(256 + i)};
    ENetPeer* peer = nullptr;
    bool sent = false, step_out = false, sync_out = false, step_in = false, sync_in = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
      ENetEvent event;
      if (enet_host_service(server.get(), &event, 5) > 0) {
        if (event.type == ENET_EVENT_TYPE_CONNECT) peer = event.peer;
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
          auto* packet = event.packet;
          int id = packet->dataLength ? packet->data[0] : 0;
          if (id == NP_MSG_SLIPPI_COMPLETE_STEP || id == NP_MSG_SLIPPI_SYNCED_STATE) {
            assert(event.channelID == 0 && (packet->flags & ENET_PACKET_FLAG_RELIABLE));
            const std::vector<uint8_t> bytes(packet->data, packet->data + packet->dataLength);
            assert(bytes == stock_packet(id, 0));
            if (id == NP_MSG_SLIPPI_COMPLETE_STEP) step_out = true;
            else sync_out = true;
          }
          enet_packet_destroy(packet);
        }
      }
      if (peer && !sent && client.GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED) {
        client.SendGamePrepStep(step);
        client.SendSyncedGameState(state);
        for (auto id : {NP_MSG_SLIPPI_COMPLETE_STEP, NP_MSG_SLIPPI_SYNCED_STATE}) {
          const auto bytes = stock_packet(id, 1, -10);
          auto* payload = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
          assert(enet_peer_send(peer, 0, payload) == 0);
        }
        enet_host_flush(server.get());
        sent = true;
      }
      if (sent && !step_in) {
        const auto response = client.FetchGamePrepStep(step[0]);
        if (response[0]) {
          assert((response == std::array<uint8_t, 6>{1, 0, 12, 3, 5, 2}));
          step_in = true;
        }
      }
      if (sent && !sync_in) {
        const auto result = client.GetDesyncRecoveryState();
        if (result.is_recovering && !result.is_waiting) {
          assert(!result.is_error && result.state.fighters[0].current_health == 246);
          assert(result.state.seconds_remaining == state.seconds_remaining);
          sync_in = true;
        }
      }
      if (step_in && step_out && sync_in && sync_out) break;
    }
    assert(sent && step_in && step_out && sync_in && sync_out);
    enet_peer_disconnect_now(peer, 0);
  }
  server.reset();
  enet_deinitialize();
  std::puts("PASS: Ranked setup and recovery on reliable ENet loopback, both directions.");
}
#endif
