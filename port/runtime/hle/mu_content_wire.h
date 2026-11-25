// Custom fighters online: the wire message (NP_MSG_MU_CONTENT, 0xEC). Nucleus only, pure and
// header-only (port/tests/mu_content_wire_test.cpp runs it without the game). The design is
// docs/online-custom-content.md.
//
// Each machine tells every other player which added fighters it has installed, by content hash (the
// package as the match reads it, host/online_fighters.h), each with the id it calls that fighter by
// (26 + its place in that machine's roster, which follows install order and differs per machine).
// A fighter is selectable online only while every player's list has its content hash; the 0x82 of a
// pick of one carries the picker's id, and the receiver maps it to its own fighter through the
// picker's list. Nothing is ever downloaded.
//
// Sent only to a peer whose build message (0xE0) set kCaps4CustomFighters in the fourth capability
// byte (after the product word and its version): a peer without it never receives this message, and
// never sees an added fighter's id (the picker cannot lock one in while such a peer is there).
// Sent when that peer's build message arrives, and again whenever this machine's installed set changes.
//
// Payload after the id byte:
//   'M' 'U' 'C' version(1) kind
//   kind 1 HAVE: sender_port count(0..16) then count x { id(u8, 26..41, increasing) content_sha256[32] }
//   (kind 2 is kept for the stages' list: docs/online-custom-content.md)
//
// decode_have() refuses anything but exactly that: a short or long message, another magic, version or
// kind, a port past 3, more than 16 fighters, an id outside 26..41 or out of order, a zero hash.
// bound_to() adds the receiver's rule: the sender port is the ENet sender's.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::content_wire {

constexpr uint8_t kMessageId = 0xEC;
constexpr uint8_t kCaps4CustomFighters = 0x02;   // MU_BUILD fourth capability byte: takes NP_MSG_MU_CONTENT
constexpr uint8_t kVersion = 1;
constexpr uint8_t kKindHave = 1;
constexpr uint8_t kFirstId = 26, kMaxFighters = 16;   // installed fighters' ids: 26 .. 41
constexpr size_t kHaveHeaderBytes = 7;
constexpr size_t kHaveEntryBytes = 1 + 32;
constexpr size_t kMaxPayloadBytes = kHaveHeaderBytes + kHaveEntryBytes * kMaxFighters;

using Sha256 = std::array<uint8_t, 32>;

struct HaveEntry {
  uint8_t id = 0;            // the sender's id for this fighter, as its 0x82 would carry it
  Sha256 content{};
  bool operator==(const HaveEntry& o) const { return id == o.id && content == o.content; }
};
struct Have {
  uint8_t sender_port = 0;
  std::vector<HaveEntry> entries;
};

inline std::string hex(const Sha256& h) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : h) { out += digits[b >> 4]; out += digits[b & 15]; }
  return out;
}

namespace detail {
inline bool zero(const Sha256& h) {
  for (const uint8_t b : h) if (b) return false;
  return true;
}
inline const char* have_problem(const Have& h) {
  if (h.sender_port > 3) return "sender port past 3";
  if (h.entries.size() > kMaxFighters) return "more than 16 fighters";
  int last = -1;
  for (const HaveEntry& e : h.entries) {
    if (e.id < kFirstId || e.id >= kFirstId + kMaxFighters) return "id outside 26..41";
    if ((int)e.id <= last) return "ids out of order";
    if (zero(e.content)) return "zero hash";
    last = e.id;
  }
  return nullptr;
}
}  // namespace detail

// The payload after the id byte; empty when the list breaks a rule decode_have() enforces.
inline std::vector<uint8_t> encode_have(const Have& h) {
  std::vector<uint8_t> out;
  if (detail::have_problem(h)) return out;
  out = {'M', 'U', 'C', kVersion, kKindHave, h.sender_port, (uint8_t)h.entries.size()};
  for (const HaveEntry& e : h.entries) {
    out.push_back(e.id);
    out.insert(out.end(), e.content.begin(), e.content.end());
  }
  return out;
}

// The kind of a payload whose header is sound (0 when it is not one this build reads).
inline uint8_t kind_of(const uint8_t* data, size_t size) {
  if (!data || size < 5 || data[0] != 'M' || data[1] != 'U' || data[2] != 'C' || data[3] != kVersion) return 0;
  return data[4] == kKindHave ? data[4] : 0;
}

inline bool decode_have(const uint8_t* data, size_t size, Have* out, std::string* why) {
  const auto fail = [why](const char* w) { if (why) *why = w; return false; };
  if (!data || size < kHaveHeaderBytes) return fail("shorter than its header");
  if (data[0] != 'M' || data[1] != 'U' || data[2] != 'C') return fail("not an MUC message");
  if (data[3] != kVersion) return fail("unknown version");
  if (data[4] != kKindHave) return fail("not a fighter list");
  const size_t count = data[6];
  if (count > kMaxFighters) return fail("more than 16 fighters");
  if (size != kHaveHeaderBytes + kHaveEntryBytes * count) return fail("length does not match its count");
  Have h;
  h.sender_port = data[5];
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = data + kHaveHeaderBytes + kHaveEntryBytes * i;
    HaveEntry e;
    e.id = p[0];
    std::copy(p + 1, p + 33, e.content.begin());
    h.entries.push_back(e);
  }
  if (const char* problem = detail::have_problem(h)) return fail(problem);
  *out = std::move(h);
  return true;
}

// The receiver's rule: a machine speaks only for its own port.
inline bool bound_to(const Have& h, int sender_port, std::string* why) {
  if (h.sender_port != sender_port) { if (why) *why = "names a port that is not the sender's"; return false; }
  return true;
}

}  // namespace slippi::content_wire
