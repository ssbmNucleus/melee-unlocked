// Opponents' skins online: the wire message (NP_MSG_MU_SKINS, 0xEA). Melee Unlocked only, pure and
// header-only (port/tests/mu_skin_wire_test.cpp runs it without the game).
//
// Each machine names the skin its own player wears for the pick it is about to send, by its SSBM
// Nucleus ids and the SHA-256 of the file the site serves. The receiver fetches it, checks the hash,
// and builds the served form against its own retail disc (host/opponent_skins.h). Nothing about
// the skin is agreed: each side decides alone what it draws, the simulation never reads it.
//
// Sent just before each 0x82 (match selections) on the same reliable channel, and only to a peer
// whose build message (0xE0) set kCaps3Skins in the third capability byte; the build message's
// version is unchanged, and a build without the bit never sees the id. Kept out of the setup hash,
// the layer fingerprint and the mod view.
//
// Payload after the id byte, big-endian, fixed size:
//   'M' 'U' 'S' version(1) sender_port count(0..4)
//   count x 84-byte record:
//     port character costume flags(0x01 partner)
//     mod_id(u32) file_id(u32) sha256[32]
//     partner_mod_id(u32) partner_file_id(u32) partner_sha256[32]   (all zero without the flag)
// The partner is the other costume file of a two-file pick: Nana for the Ice Climbers, Sheik for
// Zelda, Zelda for Sheik. `character` is the character select's number (0 Captain Falcon to 25
// Ganondorf, as 0x82 sends it) and `costume` the costume index, as 0x82 sends them.
//
// decode() refuses anything but exactly that: a short or long message, another magic or version,
// more than four records, a port past 3 or twice, a character past 25, a costume past 5, unknown
// flag bits, a zero id or hash, a partner on a fighter with one file, or partner bytes without the
// flag. bound_to() adds the receiver's rule: the header's port is the ENet sender's, and so is every
// record's (a machine names only its own player's skin).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::skin_wire {

constexpr uint8_t kMessageId = 0xEA;
constexpr uint8_t kCaps3Skins = 0x80;   // MU_BUILD third capability byte: takes NP_MSG_MU_SKINS
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxRecords = 4;
constexpr size_t kHeaderBytes = 6;
constexpr size_t kRecordBytes = 84;
constexpr size_t kMaxPayloadBytes = kHeaderBytes + kMaxRecords * kRecordBytes;
constexpr uint8_t kFlagPartner = 0x01;
constexpr uint8_t kCharacterCount = 26;
constexpr uint8_t kCostumeCount = 6;

using Sha256 = std::array<uint8_t, 32>;
struct FileRef {
  uint32_t mod_id = 0, file_id = 0;
  Sha256 sha256{};
};
struct Record {
  uint8_t port = 0, character = 0, costume = 0;
  FileRef file;
  bool has_partner = false;
  FileRef partner;
};
struct Message {
  uint8_t sender_port = 0;
  std::vector<Record> records;
};

// The fighters whose pick loads two costume files: Ice Climbers (14), Zelda (18), Sheik (19).
inline bool two_file_character(uint8_t character) { return character == 14 || character == 18 || character == 19; }

inline std::string hex(const Sha256& h) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : h) { out += digits[b >> 4]; out += digits[b & 15]; }
  return out;
}
// 64 lower-case hex digits only.
inline bool from_hex(const std::string& text, Sha256* out) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    uint8_t v = 0;
    for (int k = 0; k < 2; ++k) {
      const char c = text[i * 2 + k];
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (d < 0) return false;
      v = (uint8_t)(v << 4 | d);
    }
    (*out)[i] = v;
  }
  return true;
}

namespace detail {
inline bool zero(const Sha256& h) {
  for (const uint8_t b : h) if (b) return false;
  return true;
}
inline void put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16)); out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v);
}
inline uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline bool file_ok(const FileRef& f) { return f.mod_id && f.file_id && !zero(f.sha256); }
inline bool file_empty(const FileRef& f) { return !f.mod_id && !f.file_id && zero(f.sha256); }
inline bool record_ok(const Record& r, std::string* why) {
  const char* w = r.port > 3 ? "port past 3"
                : r.character >= kCharacterCount ? "character past 25"
                : r.costume >= kCostumeCount ? "costume past 5"
                : !file_ok(r.file) ? "zero id or hash"
                : r.has_partner && !two_file_character(r.character) ? "partner on a one-file fighter"
                : r.has_partner && !file_ok(r.partner) ? "zero partner id or hash"
                : !r.has_partner && !file_empty(r.partner) ? "partner bytes without the flag"
                : nullptr;
  if (w && why) *why = w;
  return !w;
}
}  // namespace detail

// The payload after the id byte; empty when the message breaks a rule decode() enforces.
inline std::vector<uint8_t> encode(const Message& m) {
  std::vector<uint8_t> out;
  if (m.sender_port > 3 || m.records.size() > kMaxRecords) return out;
  uint8_t seen = 0;
  for (const Record& r : m.records) {
    if (!detail::record_ok(r, nullptr) || (seen & (1u << r.port))) return out;
    seen |= (uint8_t)(1u << r.port);
  }
  out = {'M', 'U', 'S', kVersion, m.sender_port, (uint8_t)m.records.size()};
  for (const Record& r : m.records) {
    out.push_back(r.port); out.push_back(r.character); out.push_back(r.costume);
    out.push_back(r.has_partner ? kFlagPartner : 0);
    detail::put32(out, r.file.mod_id); detail::put32(out, r.file.file_id);
    out.insert(out.end(), r.file.sha256.begin(), r.file.sha256.end());
    detail::put32(out, r.partner.mod_id); detail::put32(out, r.partner.file_id);
    out.insert(out.end(), r.partner.sha256.begin(), r.partner.sha256.end());
  }
  return out;
}

// `data`: the payload after the id byte.
inline bool decode(const uint8_t* data, size_t size, Message* out, std::string* why) {
  const auto fail = [why](const char* w) { if (why) *why = w; return false; };
  if (!data || size < kHeaderBytes) return fail("shorter than its header");
  if (data[0] != 'M' || data[1] != 'U' || data[2] != 'S') return fail("not an MUS message");
  if (data[3] != kVersion) return fail("unknown version");
  if (data[4] > 3) return fail("sender port past 3");
  const size_t count = data[5];
  if (count > kMaxRecords) return fail("more than four records");
  if (size != kHeaderBytes + count * kRecordBytes) return fail("length does not match its record count");
  Message m;
  m.sender_port = data[4];
  uint8_t seen = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = data + kHeaderBytes + i * kRecordBytes;
    Record r;
    r.port = p[0]; r.character = p[1]; r.costume = p[2];
    if (p[3] & ~kFlagPartner) return fail("unknown flag bits");
    r.has_partner = (p[3] & kFlagPartner) != 0;
    r.file.mod_id = detail::get32(p + 4); r.file.file_id = detail::get32(p + 8);
    std::copy(p + 12, p + 44, r.file.sha256.begin());
    r.partner.mod_id = detail::get32(p + 44); r.partner.file_id = detail::get32(p + 48);
    std::copy(p + 52, p + 84, r.partner.sha256.begin());
    std::string w;
    if (!detail::record_ok(r, &w)) { if (why) *why = "record " + std::to_string(i) + ": " + w; return false; }
    if (seen & (1u << r.port)) return fail("a port named twice");
    seen |= (uint8_t)(1u << r.port);
    m.records.push_back(r);
  }
  *out = std::move(m);
  return true;
}

// The receiver's binding: the message came over the connection that negotiated `enet_port`, and
// names only that player.
inline bool bound_to(const Message& m, int enet_port, std::string* why) {
  if (enet_port < 0 || m.sender_port != enet_port) { if (why) *why = "header port is not the sender's"; return false; }
  for (const Record& r : m.records)
    if (r.port != m.sender_port) { if (why) *why = "a record for another player's port"; return false; }
  return true;
}

}  // namespace slippi::skin_wire
