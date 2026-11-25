// Per-player effect files, the host's half of the prototype (player_fx_test.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "player_fx_test.h"

#include "host.h"
#include "mod_data_safety.h"
#include "mod_scan.h"
#include "test_env.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace host::player_fx {
namespace {
constexpr size_t kMaxBytes = 4u << 20;   // the largest retail fighter effect file is 0.2 MB

struct Shape {
  std::string root, why;
  uint32_t models = 0, first = 0, count = 0, groups = 0;
};

uint32_t be32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

// An effect file's table ("eff...DataTable"): its particle banks and the count of model effect records
// after them (0x14 bytes each, up to the next block anything points at, as efdat.py counts them).
bool shape_of(const std::vector<uint8_t>& b, const std::string& want_root, Shape* s) {
  if (b.size() < 32 || be32(b, 0) != b.size()) { s->why = "not an archive"; return false; }
  const uint64_t length = be32(b, 4), relocs = be32(b, 8), roots = be32(b, 12), refs = be32(b, 16);
  const uint64_t reloc_at = 32 + length, root_at = reloc_at + relocs * 4, names = root_at + (roots + refs) * 8;
  if (length > b.size() || relocs > length / 4 || roots > 1024 || refs > 1024 || names > b.size()) {
    s->why = "its archive header does not hold together";
    return false;
  }
  auto word = [&](uint64_t at) { return be32(b, size_t(32 + at)); };
  std::set<uint32_t> starts{uint32_t(length)};
  for (uint64_t i = 0; i < relocs; ++i) {
    const uint32_t field = be32(b, size_t(reloc_at + i * 4));
    if ((field & 3) || uint64_t(field) + 4 > length) { s->why = "a relocation lies outside the file"; return false; }
    starts.insert(word(field));
  }
  uint32_t table = 0;
  bool found = false;
  for (uint64_t i = 0; i < roots; ++i) {
    const uint32_t at = be32(b, size_t(root_at + i * 8));
    const uint64_t name = names + be32(b, size_t(root_at + i * 8 + 4));
    starts.insert(at);
    if (name >= b.size()) continue;
    const std::string text(reinterpret_cast<const char*>(b.data() + name), strnlen(reinterpret_cast<const char*>(b.data() + name), b.size() - size_t(name)));
    const bool is_table = want_root.empty() ? (text.size() > 9 && text.compare(text.size() - 9, 9, "DataTable") == 0) : text == want_root;
    if (is_table && !found) { table = at; s->root = text; found = true; }
  }
  if (!found) { s->why = "it has no " + (want_root.empty() ? std::string("effect table") : want_root); return false; }
  const auto next = starts.upper_bound(table);
  const uint32_t end = next == starts.end() ? uint32_t(length) : *next;
  if (uint64_t(table) + 8 > length || end < table + 8) { s->why = "its effect table is cut short"; return false; }
  s->models = (end - table - 8) / 0x14;
  const uint32_t cmd = word(table), tex = word(table + 4);
  if (uint64_t(cmd) + 12 > length || uint64_t(tex) + 4 > length) { s->why = "its particle banks lie outside the file"; return false; }
  const uint32_t version = word(cmd) >> 16;
  s->first = version == 0 ? 0 : word(cmd + 4);
  s->count = version == 0 ? word(cmd + 4) : word(cmd + 8);
  s->groups = word(tex);
  return true;
}

struct Entry {
  std::string path;
  bool checked = false;
  std::string file;                       // the disc file it was checked against
  std::vector<uint8_t> bytes;             // empty: refused
};

std::mutex g_mutex;
bool g_parsed = false;
std::map<int, Entry> g_entries;   // by port (0-3)

void parse_switch() {
  if (g_parsed) return;
  g_parsed = true;
  const char* text = host::test_env::automated_getenv("MELEE_TEST_PLAYER_FX");
  if (!text || !*text) return;
  std::string all(text);
  size_t at = 0;
  while (at <= all.size()) {
    const size_t stop = std::min(all.find(';', at), all.size());
    const std::string item = all.substr(at, stop - at);
    at = stop + 1;
    if (item.size() < 3 || item[1] != ':' || item[0] < '1' || item[0] > '4') {
      if (!item.empty()) host::log("player fx: \"%s\" is not PORT:PATH; ignored", item.c_str());
      continue;
    }
    g_entries[item[0] - '1'].path = item.substr(2);
    host::log("player fx: test switch: port %c uses %s", item[0], item.substr(2).c_str());
  }
}

// The variant for `port`, checked once against the disc's `file`; null when there is none.
const std::vector<uint8_t>* variant(int port, const std::string& file) {
  const auto it = g_entries.find(port);
  if (it == g_entries.end()) return nullptr;
  Entry& e = it->second;
  if (e.checked) return e.file == file && !e.bytes.empty() ? &e.bytes : nullptr;
  e.checked = true;
  e.file = file;
  std::vector<uint8_t> bytes, retail;
  {
    std::ifstream in(std::filesystem::u8path(e.path), std::ios::binary);
    if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::string disc = host::options.mod_base_iso.empty() ? host::options.iso : host::options.mod_base_iso;
  Shape want, got;
  std::string why;
  if (bytes.empty() || bytes.size() > kMaxBytes) {
    why = "it cannot be read or is over 4 MB";
  } else if (!source_port::mods::read_disc_file(std::filesystem::u8path(disc), ("/" + file).c_str(), &retail) ||
             !shape_of(retail, "", &want)) {
    why = "the disc's " + file + " cannot be read";
  } else if (!shape_of(bytes, want.root, &got)) {
    why = got.why;
  } else if (got.models != want.models || got.first != want.first || got.count != want.count || got.groups != want.groups) {
    why = "its counts differ from the disc's (models " + std::to_string(got.models) + "/" + std::to_string(want.models) +
          ", generators " + std::to_string(got.first) + "+" + std::to_string(got.count) + "/" + std::to_string(want.first) +
          "+" + std::to_string(want.count) + ", texture groups " + std::to_string(got.groups) + "/" +
          std::to_string(want.groups) + ")";
  } else {
    host::cosmetics::disc_metadata::Archive archive(bytes);
    if (!archive.parse()) why = "it fails the archive check";
    else if (!host::cosmetics::data_safety::effect_banks_valid(archive, &why)) {}
  }
  if (!why.empty()) {
    host::log("player fx: port %d: %s refused for %s: %s", port + 1, e.path.c_str(), file.c_str(), why.c_str());
    return nullptr;
  }
  e.bytes = std::move(bytes);
  host::log("player fx: port %d: %s (%zu bytes) served for %s (%s: %u model effects, generators %u-%u, %u texture "
            "groups, as the disc's)",
            port + 1, e.path.c_str(), e.bytes.size(), file.c_str(), got.root.c_str(), got.models, got.first,
            got.first + got.count - 1, got.groups);
  return &e.bytes;
}
}  // namespace

void command(const uint8_t* p, size_t n, bool allowed, std::vector<uint8_t>& reply) {
  reply = {0};
  if (n < 5 || n > 4 + 31 || p[2] > 3) return;
  std::string file;
  for (size_t i = 4; i < n; ++i) {
    if (p[i] < 33 || p[i] > 126 || p[i] == '/' || p[i] == '\\') return;
    file.push_back(char(p[i]));
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  parse_switch();
  if (g_entries.empty()) return;
  if (!allowed) {
    static bool logged = false;
    if (!logged) host::log("player fx: online, a p2p session or a replay: every port plays its disc file");
    logged = true;
    return;
  }
  const std::vector<uint8_t>* bytes = variant(p[2], file);
  if (!bytes) return;
  if (p[1] == 0) {
    const uint32_t size = uint32_t(bytes->size());
    reply = {1, uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
  } else if (p[1] == 1) {
    reply.assign(bytes->begin(), bytes->end());
  }
}
}  // namespace host::player_fx
