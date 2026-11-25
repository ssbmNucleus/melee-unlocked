// The opponents' skins message (hle/mu_skin_wire.h): round trips, the exact sizes, and every
// malformed form decode() refuses, plus the receiver's binding to the sender's port.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mu_skin_wire.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace w = slippi::skin_wire;
int g_failures = 0;
#define CHECK(cond, what)                                                                          \
  do {                                                                                             \
    if (!(cond)) { ++g_failures; std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); }     \
    else std::printf("ok   %s\n", what);                                                           \
  } while (0)

w::Sha256 hash_of(uint8_t seed) {
  w::Sha256 h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = (uint8_t)(seed * 31 + i * 7 + 1);
  return h;
}
w::Record record(uint8_t port, uint8_t character, uint8_t costume) {
  w::Record r;
  r.port = port; r.character = character; r.costume = costume;
  r.file = {16793, 4012, hash_of(port)};
  return r;
}
bool decodes(const std::vector<uint8_t>& bytes, std::string* why = nullptr) {
  w::Message m;
  std::string ignored;
  return w::decode(bytes.data(), bytes.size(), &m, why ? why : &ignored);
}
std::string reason(const std::vector<uint8_t>& bytes) {
  std::string why;
  decodes(bytes, &why);
  return why;
}
}  // namespace

int main() {
  // Round trips.
  w::Message one{1, {record(1, 2, 3)}};   // port 1, Fox, Green
  auto bytes = w::encode(one);
  CHECK(bytes.size() == w::kHeaderBytes + w::kRecordBytes, "one record: 6 + 84 bytes");
  w::Message back;
  std::string why;
  CHECK(w::decode(bytes.data(), bytes.size(), &back, &why) && back.sender_port == 1 && back.records.size() == 1 &&
            back.records[0].character == 2 && back.records[0].costume == 3 && back.records[0].file.mod_id == 16793 &&
            back.records[0].file.file_id == 4012 && back.records[0].file.sha256 == hash_of(1) && !back.records[0].has_partner,
        "one record round trip");
  CHECK(bytes[0] == 'M' && bytes[1] == 'U' && bytes[2] == 'S' && bytes[3] == 1 && bytes[4] == 1 && bytes[5] == 1, "header bytes");
  CHECK(bytes[10] == 0x00 && bytes[11] == 0x00 && bytes[12] == 0x41 && bytes[13] == 0x99, "mod id big-endian");

  w::Message none{2, {}};
  bytes = w::encode(none);
  CHECK(bytes.size() == w::kHeaderBytes && decodes(bytes), "no record (this player wears no skin): header only");

  w::Record ic = record(0, 14, 1);   // Ice Climbers, Nana's file as the partner
  ic.has_partner = true;
  ic.partner = {16793, 4013, hash_of(9)};
  bytes = w::encode({0, {ic}});
  CHECK(w::decode(bytes.data(), bytes.size(), &back, &why) && back.records[0].has_partner && back.records[0].partner.file_id == 4013 &&
            back.records[0].partner.sha256 == hash_of(9),
        "Ice Climbers with a partner file round trip");

  w::Message four{0, {record(0, 0, 0), record(1, 9, 4), record(2, 18, 2), record(3, 25, 5)}};
  bytes = w::encode(four);
  CHECK(bytes.size() == w::kMaxPayloadBytes && decodes(bytes), "four records: the most, 342 bytes");

  // Encode refuses what decode would.
  CHECK(w::encode({0, {record(0, 2, 0), record(1, 2, 0), record(2, 2, 0), record(3, 2, 0), record(0, 2, 1)}}).empty(), "encode: five records refused");
  CHECK(w::encode({4, {}}).empty(), "encode: sender port 4 refused");
  CHECK(w::encode({0, {record(0, 26, 0)}}).empty(), "encode: character 26 refused");
  w::Record fox_partner = record(0, 2, 0);
  fox_partner.has_partner = true; fox_partner.partner = {1, 1, hash_of(3)};
  CHECK(w::encode({0, {fox_partner}}).empty(), "encode: partner on Fox refused");

  // Malformed messages.
  const auto good = w::encode(one);
  std::printf("     malformed:\n");
  const auto refused = [&](std::vector<uint8_t> b, const char* what) {
    std::string r;
    const bool ok = decodes(b, &r);
    CHECK(!ok, what);
    if (!ok) std::printf("       -> %s\n", r.c_str());
  };
  refused({}, "empty");
  refused({'M', 'U', 'S', 1, 0}, "five bytes");
  refused(std::vector<uint8_t>(good.begin(), good.end() - 1), "one byte short");
  {
    auto b = good; b.push_back(0);
    refused(b, "one byte long");
  }
  {
    auto b = good; b[2] = 'B';
    refused(b, "wrong magic");
  }
  {
    auto b = good; b[3] = 2;
    refused(b, "version 2");
  }
  {
    auto b = good; b[4] = 4;
    refused(b, "sender port 4");
  }
  {
    auto b = w::encode(four); b[5] = 5;
    b.resize(w::kHeaderBytes + 5 * w::kRecordBytes, 0);
    refused(b, "five records");
  }
  {
    auto b = good; b[5] = 2;
    refused(b, "count 2 with one record's bytes");
  }
  {
    auto b = good; b[5] = 0;
    refused(b, "count 0 with a record's bytes");
  }
  {
    auto b = good; b[6] = 4;
    refused(b, "record port 4");
  }
  {
    auto b = good; b[7] = 26;
    refused(b, "character 26");
  }
  {
    auto b = good; b[8] = 6;
    refused(b, "costume 6");
  }
  {
    auto b = good; b[9] = 0x02;
    refused(b, "unknown flag bit");
  }
  {
    auto b = good; b[9] = 0x01;
    refused(b, "partner flag on Fox");
  }
  {
    auto b = good; for (int i = 10; i < 14; ++i) b[i] = 0;
    refused(b, "mod id 0");
  }
  {
    auto b = good; for (int i = 14; i < 18; ++i) b[i] = 0;
    refused(b, "file id 0");
  }
  {
    auto b = good; for (int i = 18; i < 50; ++i) b[i] = 0;
    refused(b, "hash all zero");
  }
  {
    auto b = good; b[w::kHeaderBytes + 60] = 1;
    refused(b, "partner bytes without the flag");
  }
  {
    auto b = w::encode({0, {ic}}); for (int i = 50; i < 54; ++i) b[i] = 0;
    refused(b, "partner flag with a zero partner id");
  }
  {
    auto b = w::encode({0, {record(1, 2, 0), record(2, 2, 0)}}); b[w::kHeaderBytes + w::kRecordBytes] = 1;
    refused(b, "one port named twice");
  }

  // The receiver's binding to the ENet sender.
  CHECK(w::bound_to(one, 1, &why), "bound: header and record are the sender's port");
  CHECK(!w::bound_to(one, 2, &why), "not bound: header names another port than the sender");
  CHECK(!w::bound_to(one, -1, &why), "not bound: sender unknown");
  CHECK(!w::bound_to({1, {record(1, 2, 0), record(2, 2, 0)}}, 1, &why), "not bound: a record for another player");
  CHECK(w::bound_to({3, {}}, 3, &why), "bound: no records");

  // Hex.
  w::Sha256 h{};
  CHECK(w::from_hex(w::hex(hash_of(5)), &h) && h == hash_of(5), "hex round trip");
  CHECK(!w::from_hex(std::string(63, 'a'), &h) && !w::from_hex(std::string(64, 'A'), &h) && !w::from_hex(std::string(64, 'g'), &h),
        "hex: wrong length, upper case and non-hex refused");

  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
