// Host<->game payloads that are plain structs with a byte codec in port/runtime/abi: encode and
// decode are exact inverses, the byte layout is the documented one, and every malformed form is
// refused. Live pick previews (0xED, mu_pick_preview.h: the wire and the game's read reply) and the
// Squad Strike lineup (0xEC/0xE3, mu_squad_wire.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#define MU_SQUAD 1
#include "mu_pick_preview.h"
#include "mu_squad_wire.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(cond, what)                                                                          \
  do {                                                                                             \
    if (!(cond)) { ++g_failures; std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); }     \
  } while (0)

// A fixed sequence, so a failure names the same case on every run.
uint32_t g_state = 0x5EED1234u;
uint32_t next() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 17;
  g_state ^= g_state << 5;
  return g_state;
}
unsigned below(unsigned n) { return next() % n; }

typedef std::vector<unsigned char> Bytes;

// ---- live pick previews ----

MuPickPreview random_pick() {
  MuPickPreview p;
  mu_pick_preview_clear(&p);
  p.char_id = below(4) == 0 ? MU_PICK_PREVIEW_NONE : (unsigned char)below(33);
  p.color = (unsigned char)below(6);
  p.team_id = below(2) ? MU_PICK_PREVIEW_NONE : (unsigned char)below(3);
  p.squad_count = (unsigned char)below(MU_PICK_PREVIEW_SLOTS + 1);
  for (unsigned i = 0; i < p.squad_count; ++i) {
    p.kinds[i] = (unsigned char)below(MU_PICK_PREVIEW_NONE);   // any fighter but NONE
    p.colors[i] = (unsigned char)below(6);
  }
  p.locked = (unsigned char)below(2);
  return p;
}

Bytes encode_pick(const MuPickPreview& p) {
  Bytes out(MU_PICK_PREVIEW_WIRE);
  out.resize(mu_pick_preview_encode(&p, out.data(), out.size()));
  return out;
}

bool same_pick(const MuPickPreview& a, const MuPickPreview& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

void pick_preview_wire() {
  // The documented layout, byte for byte: version, char, color, team, count, 3 x {kind, color}, locked.
  MuPickPreview fox;
  mu_pick_preview_clear(&fox);
  fox.char_id = 2; fox.color = 1; fox.squad_count = 2;
  fox.kinds[0] = 9; fox.colors[0] = 1; fox.kinds[1] = 20; fox.locked = 1;
  const Bytes golden = {1, 2, 1, 0xFF, 2, 9, 1, 20, 0, 0xFF, 0, 1};
  CHECK(golden.size() == MU_PICK_PREVIEW_WIRE, "pick preview: the wire is 12 bytes");
  CHECK(encode_pick(fox) == golden, "pick preview: the documented byte layout");

  for (int n = 0; n < 2000; ++n) {
    const MuPickPreview p = random_pick();
    const Bytes bytes = encode_pick(p);
    MuPickPreview back;
    CHECK(bytes.size() == MU_PICK_PREVIEW_WIRE, "pick preview: a valid pick encodes to the whole wire");
    CHECK(mu_pick_preview_decode(bytes.data(), bytes.size(), &back) && same_pick(p, back),
          "pick preview: decode(encode(pick)) is the pick");
  }

  // Any 12 bytes decode either to nothing or to a pick that encodes back to exactly those bytes.
  int accepted = 0;
  for (int n = 0; n < 20000; ++n) {
    Bytes bytes(MU_PICK_PREVIEW_WIRE);
    for (auto& b : bytes) b = (unsigned char)(below(4) ? below(256) : (below(2) ? 0 : 0xFF));
    bytes[0] = MU_PICK_PREVIEW_VERSION;
    MuPickPreview p;
    if (!mu_pick_preview_decode(bytes.data(), bytes.size(), &p)) continue;
    ++accepted;
    CHECK(encode_pick(p) == bytes, "pick preview: encode(decode(bytes)) is the bytes");
  }
  CHECK(accepted > 0, "pick preview: some random wires are canonical");

  // Refused: the wrong size or version, and every non-canonical field.
  MuPickPreview out;
  Bytes bad = golden;
  CHECK(!mu_pick_preview_decode(golden.data(), golden.size() - 1, &out), "pick preview: one byte short is refused");
  bad.push_back(0);
  CHECK(!mu_pick_preview_decode(bad.data(), bad.size(), &out), "pick preview: a trailing byte is refused");
  struct Case { unsigned at; unsigned char value; const char* what; };
  const Case cases[] = {
    {0, 2, "pick preview: another version is refused"},
    {3, 3, "pick preview: team 3 is refused"},
    {4, 4, "pick preview: four squad slots are refused"},
    {5, 0xFF, "pick preview: a filled slot without a fighter is refused"},
    {9, 7, "pick preview: an empty slot naming a fighter is refused"},
    {10, 1, "pick preview: an empty slot with a color is refused"},
    {11, 2, "pick preview: locked 2 is refused"},
  };
  for (const Case& c : cases) {
    bad = golden;
    bad[c.at] = c.value;
    CHECK(!mu_pick_preview_decode(bad.data(), bad.size(), &out), c.what);
  }
  unsigned char small[MU_PICK_PREVIEW_WIRE - 1];
  CHECK(mu_pick_preview_encode(&fox, small, sizeof small) == 0, "pick preview: a short buffer is refused");
  MuPickPreview odd = fox;
  odd.colors[2] = 3;   // past squad_count
  Bytes room(MU_PICK_PREVIEW_WIRE);
  CHECK(mu_pick_preview_encode(&odd, room.data(), room.size()) == 0, "pick preview: a non-canonical pick is not sent");
}

void pick_preview_reply() {
  // The game's read reply: version, then per remote {valid, port, wire}.
  for (int n = 0; n < 500; ++n) {
    MuPickPreviewSlot slots[MU_PICK_PREVIEW_REMOTES];
    std::memset(slots, 0, sizeof slots);
    for (auto& s : slots) {
      if (below(3) == 0) continue;   // an empty slot: all zero
      s.valid = 1;
      s.port = (unsigned char)below(4);
      s.pick = random_pick();
    }
    Bytes reply(MU_PICK_PREVIEW_REPLY);
    CHECK(mu_pick_preview_reply_encode(slots, reply.data(), reply.size()) == MU_PICK_PREVIEW_REPLY,
          "pick preview reply: encodes to the whole reply");
    MuPickPreviewSlot back[MU_PICK_PREVIEW_REMOTES];
    CHECK(mu_pick_preview_reply_decode(reply.data(), reply.size(), back) == 1, "pick preview reply: decodes");
    CHECK(std::memcmp(slots, back, sizeof slots) == 0, "pick preview reply: decode(encode(slots)) is the slots");

    // One bad slot is that slot alone.
    Bytes damaged = reply;
    damaged[1 + 1] = 4;   // remote 0's port
    MuPickPreviewSlot partial[MU_PICK_PREVIEW_REMOTES];
    CHECK(mu_pick_preview_reply_decode(damaged.data(), damaged.size(), partial) == 1 && !partial[0].valid &&
              std::memcmp(&partial[1], &slots[1], 2 * sizeof slots[0]) == 0,
          "pick preview reply: a bad port drops only its own slot");
  }
  MuPickPreviewSlot slots[MU_PICK_PREVIEW_REMOTES];
  Bytes reply(MU_PICK_PREVIEW_REPLY, 0);
  reply[0] = 2;
  CHECK(mu_pick_preview_reply_decode(reply.data(), reply.size(), slots) == 0 && !slots[0].valid,
        "pick preview reply: another version is refused whole");
  reply[0] = MU_PICK_PREVIEW_VERSION;
  CHECK(mu_pick_preview_reply_decode(reply.data(), reply.size() - 1, slots) == 0,
        "pick preview reply: the wrong size is refused whole");
}

// ---- Squad Strike lineup ----

MuSquadSide random_side() {
  MuSquadSide side;
  std::memset(&side, 0, sizeof side);
  side.count = MU_SQUAD_LINEUP;
  for (unsigned i = 0; i < side.count; ++i) {
    side.members[i].kind = (unsigned char)below(26);
    side.members[i].color = (unsigned char)below(mu_squad_colors(side.members[i].kind));
  }
  return side;
}

Bytes encode_squad(const MuSquadSetup& s) {
  Bytes out(MU_SQUAD_WIRE_MAX);
  out.resize(mu_squad_encode(&s, out.data(), out.size()));
  return out;
}

void squad_wire() {
  MuSquadSetup setup;
  std::memset(&setup, 0, sizeof setup);
  setup.stocks = 4;
  // {kind, color}: each color inside that kind's own count (mu_squad_colors).
  const unsigned char p1[4][2] = {{2, 1}, {9, 0}, {20, 3}, {0, 5}};
  const unsigned char p2[4][2] = {{18, 0}, {12, 2}, {7, 3}, {25, 3}};
  for (unsigned i = 0; i < 4; ++i) {
    setup.sides[0].members[i] = {p1[i][0], p1[i][1]};
    setup.sides[1].members[i] = {p2[i][0], p2[i][1]};
  }
  setup.sides[0].count = setup.sides[1].count = MU_SQUAD_LINEUP;
  const Bytes golden = {3, 4, 4, 2, 1, 9, 0, 20, 3, 0, 5, 4, 18, 0, 12, 2, 7, 3, 25, 3};
  CHECK(encode_squad(setup) == golden, "squad: the documented byte layout");

  for (int n = 0; n < 2000; ++n) {
    MuSquadSetup s;
    std::memset(&s, 0, sizeof s);
    s.sides[0] = random_side();
    s.sides[1] = random_side();
    s.stocks = MU_SQUAD_LINEUP;
    const Bytes bytes = encode_squad(s);
    MuSquadSetup back;
    CHECK(bytes.size() == MU_SQUAD_WIRE_MAX, "squad: a valid lineup encodes to the whole wire");
    CHECK(mu_squad_decode(bytes.data(), bytes.size(), &back) && std::memcmp(&s, &back, sizeof s) == 0,
          "squad: decode(encode(setup)) is the setup");
    // combine() puts each side on its port and the stock count on the longer side.
    MuSquadSetup combined;
    CHECK(mu_squad_combine(1, &s.sides[1], 0, &s.sides[0], &combined) &&
              std::memcmp(&combined, &s, sizeof s) == 0,
          "squad: combining the two sides is the setup");
  }

  MuSquadSetup out;
  struct Case { unsigned at; unsigned char value; const char* what; };
  const Case cases[] = {
    {0, 2, "squad: version 2 (three fighters) is refused"},
    {1, 3, "squad: a stock count that is not the lineup size is refused"},
    {2, 3, "squad: a three-fighter side is refused"},
    {4, 6, "squad: a color past the fighter's own is refused"},
    {11, 5, "squad: a five-fighter side is refused"},
  };
  for (const Case& c : cases) {
    Bytes bad = golden;
    bad[c.at] = c.value;
    CHECK(!mu_squad_decode(bad.data(), bad.size(), &out), c.what);
  }
  Bytes longer = golden;
  longer.push_back(0);
  CHECK(!mu_squad_decode(longer.data(), longer.size(), &out), "squad: a trailing byte is refused");
  CHECK(!mu_squad_decode(golden.data(), golden.size() - 1, &out), "squad: one byte short is refused");
  unsigned char small[MU_SQUAD_WIRE_MAX - 1];
  CHECK(mu_squad_encode(&setup, small, sizeof small) == 0, "squad: a short buffer is refused");

  // Who plays Squad Strike: only when both sides asked and both builds carry the cap.
  CHECK(mu_squad_decide(0, 0, 0, 0) == MU_SQUAD_NORMAL, "squad: nobody asked is a normal match");
  CHECK(mu_squad_decide(MU_SQUAD_CAP, MU_SQUAD_CAP, 1, 1) == MU_SQUAD_PLAY, "squad: both asked, both able");
  CHECK(mu_squad_decide(MU_SQUAD_CAP, 0, 1, 1) == MU_SQUAD_CANCEL, "squad: a peer without the cap cancels");
  CHECK(mu_squad_decide(MU_SQUAD_CAP, MU_SQUAD_CAP, 1, 0) == MU_SQUAD_CANCEL,
        "squad: a lineup on one side only cancels, never a silent 1v1");
}
}  // namespace

int main() {
  pick_preview_wire();
  pick_preview_reply();
  squad_wire();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("ok: pick preview wire and reply, Squad Strike lineup\n");
  return 0;
}
