// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_replay_stream.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// A small UBJSON reader for the metadata (objects, strings, int8/uint8/int16/int32): a value is a
// string, a number or an object of values.
struct Ub {
  std::string text;
  long long number = 0;
  bool object = false;
  std::map<std::string, Ub> fields;
  const Ub& operator[](const std::string& key) const {
    static const Ub none;
    const auto it = fields.find(key);
    return it == fields.end() ? none : it->second;
  }
  bool has(const std::string& key) const { return fields.count(key) != 0; }
};
static long long ub_int(const std::vector<uint8_t>& b, size_t& i) {
  const uint8_t t = b[i++];
  if (t == 'U') return b[i++];
  if (t == 'i') return int8_t(b[i++]);
  if (t == 'I') { const long long v = int16_t(b[i] << 8 | b[i + 1]); i += 2; return v; }
  if (t == 'l') { const long long v = int32_t(be32(&b[i])); i += 4; return v; }
  CHECK(!"unexpected UBJSON integer type");
  return 0;
}
static Ub ub_value(const std::vector<uint8_t>& b, size_t& i) {
  Ub v;
  const uint8_t t = b[i];
  if (t == '{') {
    ++i;
    v.object = true;
    while (b[i] != '}') {
      size_t n = size_t(ub_int(b, i));
      const std::string key(b.begin() + i, b.begin() + i + n);
      i += n;
      v.fields[key] = ub_value(b, i);
    }
    ++i;
  } else if (t == 'S') {
    ++i;
    const size_t n = size_t(ub_int(b, i));
    v.text.assign(b.begin() + i, b.begin() + i + n);
    i += n;
  } else {
    v.number = ub_int(b, i);
  }
  return v;
}

// Game Start payload offsets (the event's offsets less the command byte), Slippi's replay spec.
enum : size_t {
  GS_VERSION = 0x0, GS_PLAYER = 0x60 + 4, GS_NAME = 0x1A4, GS_CODE = 0x220, GS_UID = 0x248,
  GS_LANGUAGE = 0x2BC, GS_MATCH_ID = 0x2BD, GS_GAME_NUMBER = 0x2F0, GS_TIEBREAK = 0x2F4, GS_SIZE = 828,
};
struct Seat { const char* name; const char* code_sjis; const char* code_utf8; const char* uid; uint8_t character; };

// One online game as the game and the host record it: Game Start with each human port's name,
// code and UID as the game copies them from the match state block, the metadata names the host
// sets, then a few frames and Game End. Returns the .slp bytes.
static std::vector<uint8_t> online_game(const std::vector<Seat>& seats, const char* match_id, uint32_t game,
                                        uint32_t tiebreak, std::vector<uint8_t>& start) {
  start.assign(GS_SIZE, 0);
  start[0] = 3; start[1] = 20; start[2] = 0;
  for (size_t port = 0; port < 4; ++port) {
    uint8_t* player = &start[GS_PLAYER + 0x24 * port];
    if (port >= seats.size()) { player[0] = 0x21; player[1] = 3; continue; }   // empty slot
    player[0] = seats[port].character; player[1] = 0;                          // human
    std::memcpy(&start[GS_NAME + 31 * port], seats[port].name, std::strlen(seats[port].name));
    std::memcpy(&start[GS_CODE + 10 * port], seats[port].code_sjis, std::strlen(seats[port].code_sjis));
    std::memcpy(&start[GS_UID + 29 * port], seats[port].uid, std::strlen(seats[port].uid));
  }
  start[GS_LANGUAGE] = 1;
  std::memcpy(&start[GS_MATCH_ID], match_id, std::strlen(match_id));
  for (int b = 0; b < 4; ++b) start[GS_GAME_NUMBER + b] = uint8_t(game >> (24 - 8 * b));
  for (int b = 0; b < 4; ++b) start[GS_TIEBREAK + b] = uint8_t(tiebreak >> (24 - 8 * b));
  slippi::NativeReplayStream stream;
  std::map<uint8_t, slippi::NativeReplayStream::PlayerName> names;
  for (size_t port = 0; port < seats.size(); ++port) names[uint8_t(port)] = {seats[port].name, seats[port].code_utf8};
  stream.set_player_names(names);
  stream.set_nucleus_fighters({{1, std::string(64, 'b') + " Wolf"}});
  CHECK(stream.begin_slippi(start, {}, 1700000000));
  for (int32_t frame = -123; frame < -120; ++frame) {
    for (size_t port = 0; port < seats.size(); ++port) {
      std::vector<uint8_t> post(92, 0);
      for (int b = 0; b < 4; ++b) post[b] = uint8_t(uint32_t(frame) >> (24 - 8 * b));
      post[4] = uint8_t(port); post[6] = seats[port].character;
      CHECK(stream.append(0x38, post.data(), uint32_t(post.size())));
    }
  }
  const uint8_t end[6] = {2, 0xFF, 0, 1, 0xFF, 0xFF};
  CHECK(stream.append(0x39, end, sizeof end));
  return stream.encode();
}

// Checks the file's Game Start against what went in, field by field, and returns the metadata.
static Ub read_back(const std::vector<uint8_t>& file, const std::vector<uint8_t>& start) {
  const uint32_t raw_size = be32(file.data() + 11);
  const size_t game_start = 15 + 1 + file[16];   // after the command table event
  CHECK(file[game_start] == 0x36);
  CHECK(std::equal(start.begin(), start.end(), file.begin() + game_start + 1));
  size_t i = 15 + raw_size;
  const std::string key = "U\x08metadata";
  CHECK(std::equal(key.begin(), key.end(), file.begin() + i));
  i += key.size();
  const Ub meta = ub_value(file, i);
  CHECK(i + 1 == file.size() && file[i] == '}');
  return meta;
}

static void check_online_names() {
  {
    // Two players (Direct): each port's 31-byte name, 10-byte code with the full-width '#'
    // (0x81 0x94) and 29-byte UID in Game Start; match id, game and tiebreak numbers; the metadata
    // names in UTF-8 with the plain '#', by player index.
    const std::vector<Seat> seats = {{"SwagPlayer", "SWAG\x81\x94" "123", "SWAG#123", "uidSwagPlayer0000000000000a", 0x02},
                                     {"Noob", "NOOB\x81\x94" "456", "NOOB#456", "uidNoob000000000000000000b", 0x14}};
    std::vector<uint8_t> start;
    const auto file = online_game(seats, "mode.direct-2026-11-25T10:00:00.00Z-0-v3", 2, 0, start);
    CHECK(!file.empty());
    const Ub meta = read_back(file, start);
    const uint8_t* gs = file.data() + 15 + 1 + file[16] + 1;
    CHECK(std::memcmp(gs + GS_NAME, "SwagPlayer", 10) == 0 && gs[GS_NAME + 10] == 0 && gs[GS_NAME + 30] == 0);
    CHECK(std::memcmp(gs + GS_NAME + 31, "Noob", 4) == 0 && gs[GS_NAME + 31 + 4] == 0);
    const uint8_t code0[10] = {'S', 'W', 'A', 'G', 0x81, 0x94, '1', '2', '3', 0};
    const uint8_t code1[10] = {'N', 'O', 'O', 'B', 0x81, 0x94, '4', '5', '6', 0};
    CHECK(std::memcmp(gs + GS_CODE, code0, 10) == 0 && std::memcmp(gs + GS_CODE + 10, code1, 10) == 0);
    CHECK(std::memcmp(gs + GS_UID, "uidSwagPlayer0000000000000a", 27) == 0 && gs[GS_UID + 28] == 0);
    CHECK(std::memcmp(gs + GS_UID + 29, "uidNoob000000000000000000b", 26) == 0);
    for (size_t b = 0; b < 31 * 2; ++b) CHECK(gs[GS_NAME + 31 * 2 + b] == 0);   // ports 3 and 4: empty
    for (size_t b = 0; b < 10 * 2; ++b) CHECK(gs[GS_CODE + 10 * 2 + b] == 0);
    for (size_t b = 0; b < 29 * 2; ++b) CHECK(gs[GS_UID + 29 * 2 + b] == 0);
    CHECK(gs[GS_LANGUAGE] == 1);
    CHECK(std::string(reinterpret_cast<const char*>(gs + GS_MATCH_ID)) == "mode.direct-2026-11-25T10:00:00.00Z-0-v3");
    CHECK(be32(gs + GS_GAME_NUMBER) == 2 && be32(gs + GS_TIEBREAK) == 0);
    CHECK(gs[GS_VERSION] == 3 && gs[GS_VERSION + 1] == 20 && gs[GS_VERSION + 2] == 0);
    const Ub& players = meta["players"];
    CHECK(players.fields.size() == 2);
    CHECK(players["0"]["names"]["netplay"].text == "SwagPlayer" && players["0"]["names"]["code"].text == "SWAG#123");
    CHECK(players["1"]["names"]["netplay"].text == "Noob" && players["1"]["names"]["code"].text == "NOOB#456");
    CHECK(players["0"]["names"].fields.size() == 2 && players["1"]["names"].fields.size() == 2);
    CHECK(players["0"]["characters"]["2"].number == 3 && players["1"]["characters"]["20"].number == 3);
    CHECK(meta["startAt"].text == "2023-11-14T22:13:20Z" && meta["lastFrame"].number == -121);
    CHECK(meta["playedOn"].text == "dolphin");
    CHECK(meta["nucleusFighters"]["1"].text == std::string(64, 'b') + " Wolf");   // Nucleus's keys stay
  }
  {
    // Four players (Teams): every port's fields, the names on the right player indexes, and a
    // Ranked-style game and tiebreak number carried through as the game wrote them.
    const std::vector<Seat> seats = {{"Ann", "AAA\x81\x94" "1", "AAA#1", "uidAnn00000000000000000001", 0x09},
                                     {"Bob", "BBB\x81\x94" "22", "BBB#22", "uidBob00000000000000000002", 0x02},
                                     {"Cy", "CCCC\x81\x94" "333", "CCCC#333", "uidCy000000000000000000003", 0x00},
                                     {"Dee", "DDDDDDD\x81\x94" "4", "DDDDDDD#4", "uidDee00000000000000000004", 0x13}};
    std::vector<uint8_t> start;
    const auto file = online_game(seats, "mode.teams-2026-11-25T10:00:00.00Z-0-v3", 3, 1, start);
    CHECK(!file.empty());
    const Ub meta = read_back(file, start);
    const uint8_t* gs = file.data() + 15 + 1 + file[16] + 1;
    for (size_t port = 0; port < 4; ++port) {
      CHECK(std::string(reinterpret_cast<const char*>(gs + GS_NAME + 31 * port)) == seats[port].name);
      CHECK(std::memcmp(gs + GS_CODE + 10 * port, seats[port].code_sjis, std::strlen(seats[port].code_sjis)) == 0);
      CHECK(std::string(reinterpret_cast<const char*>(gs + GS_UID + 29 * port)) == seats[port].uid);
      const Ub& names = meta["players"][std::to_string(port)]["names"];
      CHECK(names["netplay"].text == seats[port].name && names["code"].text == seats[port].code_utf8);
    }
    CHECK(gs[GS_CODE + 30 + 7] == 0x81 && gs[GS_CODE + 30 + 8] == 0x94 && gs[GS_CODE + 30 + 9] == '4');   // 7+2+1: full
    CHECK(be32(gs + GS_GAME_NUMBER) == 3 && be32(gs + GS_TIEBREAK) == 1);
    CHECK(meta["players"].fields.size() == 4);
  }
  {
    // Offline: names stay {} (Dolphin writes none offline), the file is otherwise the same shape.
    slippi::NativeReplayStream stream;
    std::vector<uint8_t> start(GS_SIZE, 0);
    start[0] = 3; start[1] = 20;
    CHECK(stream.begin_slippi(start, {}, 1700000000));
    std::vector<uint8_t> post(92, 0);
    post[0] = post[1] = post[2] = 0xFF; post[3] = 0x85;
    CHECK(stream.append(0x38, post.data(), uint32_t(post.size())));
    const auto file = stream.encode();
    const Ub meta = read_back(file, start);
    CHECK(meta["players"]["0"]["names"].object && meta["players"]["0"]["names"].fields.empty());
    // An offline game after an online one: the host sets the (empty) names before every game.
    slippi::NativeReplayStream again;
    again.set_player_names({{0, {"SwagPlayer", "SWAG#123"}}});
    CHECK(again.begin_slippi(start, {}, 1700000000));
    CHECK(again.append(0x38, post.data(), uint32_t(post.size())));
    CHECK(read_back(again.encode(), start)["players"]["0"]["names"]["code"].text == "SWAG#123");
    again.set_player_names({});
    CHECK(again.begin_slippi(start, {}, 1700000000));
    CHECK(again.append(0x38, post.data(), uint32_t(post.size())));
    CHECK(read_back(again.encode(), start)["players"]["0"]["names"].fields.empty());
  }
}

int main(int argc, char** argv) {
  check_online_names();
  slippi::NativeReplayStream stream;
  std::vector<uint8_t> start(0x2F8);
  start[0] = 3; start[1] = 19;
  CHECK(!stream.begin({}, {}));
  CHECK(stream.begin(start, {}));
  const uint8_t frame[] = {255,255,255,133,0,0,0,42,0,0,0,0};
  CHECK(stream.append(0x3A, frame, sizeof frame));
  const uint8_t bookend[] = {255,255,255,133,255,255,255,133};
  CHECK(stream.append(0x3C, bookend, sizeof bookend));
  CHECK(stream.last_frame() == -123);
  auto encoded = stream.encode();
  CHECK(!encoded.empty());
  const size_t raw_end = 15 + be32(encoded.data() + 11);
  CHECK(raw_end < encoded.size());
  CHECK(encoded[raw_end] == 'U');
  std::map<uint8_t, unsigned> sizes;
  for (size_t i = 17; i < 16 + encoded[16]; i += 3)
    sizes[encoded[i]] = unsigned(encoded[i+1]) << 8 | encoded[i+2];
  CHECK(sizes[0x36] == start.size());
  CHECK(sizes.count(0x3D) && sizes[0x3D] == 0);
  CHECK(sizes[0x3A] == 12 && sizes[0x3C] == 8);
  if (argc > 1) {
    std::ofstream file(argv[1], std::ios::binary);
    file.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    CHECK(file.good());
  }
  CHECK(!stream.append(0x3A, frame, 8));
  CHECK(stream.encode().empty());
  CHECK(stream.begin(start, {}));
  CHECK(!stream.append(0x37, nullptr, 66));
  CHECK(stream.begin(start, {}));
  const uint8_t end[] = {2,255};
  CHECK(stream.append(0x39, end, sizeof end));
  CHECK(!stream.append(0x3C, bookend, sizeof bookend));
  CHECK(stream.begin(start, std::vector<uint8_t>(65536, 0xAB)));
  encoded = stream.encode();
  CHECK(encoded[16] == 7); // Start + splitter, no wrapped 16-bit Gecko size.
  CHECK(encoded[17] == 0x10 && encoded[18] == 2 && encoded[19] == 4);
  CHECK(!stream.begin(start, std::vector<uint8_t>(16 * 1024 * 1024 + 1)));
  {
    // The live copy (the Slippi spectator server) is the raw element of the .slp, byte for byte:
    // the command table, Game Start, the split Gecko list and every event, in order.
    slippi::NativeReplayStream live;
    std::vector<uint8_t> sent;
    size_t calls = 0;
    live.set_event_sink([&](const uint8_t* data, uint32_t size) { sent.insert(sent.end(), data, data + size); ++calls; });
    std::vector<uint8_t> slippi_start(0x2F8, 1);
    CHECK(live.begin_slippi(slippi_start, std::vector<uint8_t>(600, 0xCD), 1700000000));
    std::vector<uint8_t> pre(66, 2), post(92, 3), book(8, 4), game_end(6, 5);
    CHECK(live.append(0x37, pre.data(), (uint32_t)pre.size()));
    CHECK(live.append(0x38, post.data(), (uint32_t)post.size()));
    CHECK(live.append(0x3C, book.data(), (uint32_t)book.size()));
    CHECK(live.append(0x39, game_end.data(), (uint32_t)game_end.size()));
    const auto file = live.encode();
    const uint32_t raw_size = be32(file.data() + 11);
    CHECK(file.size() > 15 + raw_size);
    CHECK(sent == std::vector<uint8_t>(file.begin() + 15, file.begin() + 15 + raw_size));
    CHECK(calls == 1 + 1 + 2 + 4);   // table, Game Start, two split parts, four events
    const std::string key = "U\x0FnucleusFighters{";
    CHECK(std::search(file.begin(), file.end(), key.begin(), key.end()) == file.end());
  }
  {
    // Added fighters online: set before begin_slippi (which starts a fresh stream), written in the
    // metadata as {port: "<content hash hex> <name>"}.
    slippi::NativeReplayStream live;
    live.set_nucleus_fighters({{1, std::string(64, 'a') + " Slippy"}});
    std::vector<uint8_t> slippi_start(0x2F8, 1);
    CHECK(live.begin_slippi(slippi_start, {}, 1700000000));
    const auto file = live.encode();
    const std::string want = std::string("U\x0FnucleusFighters{U\x01" "1SU\x47") + std::string(64, 'a') + " Slippy}";
    CHECK(std::search(file.begin(), file.end(), want.begin(), want.end()) != file.end());
  }
  std::puts("native replay stream tests passed");
}
