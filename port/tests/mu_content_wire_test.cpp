// The custom fighters message (hle/mu_content_wire.h): round trips, the exact sizes, every malformed
// form decode refuses, and the receiver's binding to the sender's port.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mu_content_wire.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace w = slippi::content_wire;
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
w::Have have(uint8_t port, int count) {
  w::Have h;
  h.sender_port = port;
  for (int i = 0; i < count; ++i) h.entries.push_back({(uint8_t)(26 + i * (count < 8 ? 2 : 1)), hash_of((uint8_t)(i + 1))});
  return h;
}
std::string reason(const std::vector<uint8_t>& bytes) {
  w::Have h;
  std::string why;
  return w::decode_have(bytes.data(), bytes.size(), &h, &why) ? std::string() : why;
}
}  // namespace

int main() {
  // Round trips: none, one, three, the most.
  for (const int count : {0, 1, 3, 16}) {
    const w::Have sent = have(1, count);
    const std::vector<uint8_t> bytes = w::encode_have(sent);
    const std::string n = std::to_string(count);
    CHECK(bytes.size() == w::kHaveHeaderBytes + w::kHaveEntryBytes * count, ("have " + n + ": exact size").c_str());
    CHECK(bytes.size() <= w::kMaxPayloadBytes, ("have " + n + ": within the largest payload").c_str());
    CHECK(w::kind_of(bytes.data(), bytes.size()) == w::kKindHave, ("have " + n + ": kind_of").c_str());
    w::Have got;
    std::string why;
    CHECK(w::decode_have(bytes.data(), bytes.size(), &got, &why), ("have " + n + ": decodes").c_str());
    CHECK(got.sender_port == 1 && got.entries == sent.entries, ("have " + n + ": every entry back").c_str());
    CHECK(w::bound_to(got, 1, &why), ("have " + n + ": bound to its sender").c_str());
    CHECK(!w::bound_to(got, 2, &why), ("have " + n + ": refused from another port").c_str());
  }
  CHECK(w::kMaxPayloadBytes == 7 + 33 * 16, "largest payload is 535 bytes");

  // What encode refuses to write.
  {
    w::Have h = have(4, 1);
    CHECK(w::encode_have(h).empty(), "encode: port 4");
    h = have(0, 2);
    std::swap(h.entries[0], h.entries[1]);
    CHECK(w::encode_have(h).empty(), "encode: ids out of order");
    h = have(0, 1);
    h.entries[0].id = 25;
    CHECK(w::encode_have(h).empty(), "encode: a retail id");
    h.entries[0].id = 42;
    CHECK(w::encode_have(h).empty(), "encode: id 42");
    h = have(0, 1);
    h.entries[0].content = w::Sha256{};
    CHECK(w::encode_have(h).empty(), "encode: zero hash");
    h = have(0, 16);
    h.entries.push_back({41, hash_of(99)});
    CHECK(w::encode_have(h).empty(), "encode: 17 fighters");
  }

  // Every malformed form decode refuses.
  const std::vector<uint8_t> good = w::encode_have(have(2, 3));
  {
    std::vector<uint8_t> b = good;
    b.pop_back();
    CHECK(!reason(b).empty(), "decode: one byte short");
    b = good;
    b.push_back(0);
    CHECK(!reason(b).empty(), "decode: one byte long");
    b = good;
    b[0] = 'X';
    CHECK(!reason(b).empty(), "decode: another magic");
    b = good;
    b[3] = 2;
    CHECK(!reason(b).empty() && w::kind_of(b.data(), b.size()) == 0, "decode: another version");
    b = good;
    b[4] = 2;
    CHECK(!reason(b).empty() && w::kind_of(b.data(), b.size()) == 0, "decode: kind 2 (stages) not read by this build");
    b = good;
    b[5] = 4;
    CHECK(!reason(b).empty(), "decode: port 4");
    b = good;
    b[6] = 17;
    CHECK(!reason(b).empty(), "decode: count 17");
    b = good;
    b[6] = 2;
    CHECK(!reason(b).empty(), "decode: count says 2, three entries");
    b = good;
    b[7] = 25;
    CHECK(!reason(b).empty(), "decode: a retail id");
    b = good;
    b[7 + 33] = b[7];
    CHECK(!reason(b).empty(), "decode: a repeated id");
    b = good;
    for (int i = 0; i < 32; ++i) b[8 + i] = 0;
    CHECK(!reason(b).empty(), "decode: zero hash");
    b.assign(good.begin(), good.begin() + 3);
    CHECK(!reason(b).empty() && w::kind_of(b.data(), b.size()) == 0, "decode: three bytes");
    CHECK(!reason({}).empty(), "decode: empty");
  }
  CHECK(w::hex(hash_of(1)).size() == 64, "hex: 64 digits");

  std::printf("passed: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
