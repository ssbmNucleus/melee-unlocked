// Opponents' skins online, the receiving machine's half (opponent_skins.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "opponent_skins.h"
#include "archive_unpack.h"
#include "cosmetic_mods.h"
#include "costume_served_form.h"
#include "costume_slots.h"
#include "hackpack_ai.h"
#include "host.h"
#include "retail_costume_hashes.h"
#include "roster.h"
#include "skin_fetch.h"
#include "test_env.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "nlohmann/json.hpp"

namespace host::skins {
namespace {
namespace fs = std::filesystem;
namespace wire = slippi::skin_wire;

constexpr int kPorts = 4;
constexpr uint32_t kMaxCostumeBytes = 32u << 20;   // one costume file out of a download

std::atomic<bool> g_enabled{true};   // on by default (0.1.0); "opponentskins 0" turns it off

// The local player's port and pick in the session (local_payload), -1 before the first pick.
std::atomic<int> g_local_port{-1};

// served_costume's answers per port, and the served forms this machine built itself (the test hook,
// its own skin, the retail file), by "<source>|<slot>"; a null entry is a refusal, kept so it is
// logged once. Under g_served_mutex.
using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
struct Answer { std::string slot; Bytes bytes; };
std::mutex g_served_mutex;
Answer g_answer[kPorts];
std::map<std::string, Bytes> g_built;
// An opponent's skin in another costume of its fighter (a Teams color): its served forms by slot, for
// the pick of generation g_recolor_generation[port] (a newer pick starts the port's afresh).
std::map<std::string, Bytes> g_recolored[kPorts];
uint64_t g_recolor_generation[kPorts] = {};
// The pick whose opponent skin served_costume last handed each port's fighter (its main file: Nana's
// or Sheik's file leaves it alone), 0 none: worn_stock answers only for that pick.
uint64_t g_worn_generation[kPorts] = {};

constexpr int kStockSide = 24;   // the HUD's stock icon (shim/mu_css_skins.c), as costume_slots.h's

struct Job {
  int port = 0;
  uint64_t generation = 0;
  uint8_t character = 0, costume = 0;
  wire::Record record;
  bool stock = false;   // the ready skin's stock icon, after every port's skin
};

std::mutex g_mutex;   // everything below
std::condition_variable g_changed;   // a port's entry changed, or a job was queued
PortSkin g_ports[kPorts];
uint64_t g_generation[kPorts] = {};
std::deque<Job> g_jobs;
bool g_worker = false;

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
bool read_file(const fs::path& path, uint64_t limit, std::vector<uint8_t>* out) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  out->resize((size_t)size);
  return in && (!size || in.read((char*)out->data(), (std::streamsize)size));
}

// One file of a two-file pick, or the pick's own: fetched, taken out of its download, served form.
struct FileResult {
  bool ok = false;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  std::string summary;   // or the reason
  std::shared_ptr<const std::vector<uint8_t>> source;   // ok: the costume file it was made from
};
FileResult serve_one(const wire::FileRef& file, const std::string& slot) {
  FileResult r;
  std::vector<uint8_t> retail;
  std::string why;
  if (!retail_reference(slot, &retail, &why)) { r.summary = why; return r; }
  const std::string sha = wire::hex(file.sha256);
  const Fetched fetched = fetch_skin(file.mod_id, file.file_id, sha);
  if (!fetched.ok) { r.summary = "fetch: " + fetched.reason; return r; }
  std::vector<uint8_t> dat;
  const fs::path scratch = fs::u8path(cache_folder()) / fs::u8path(sha + ".unpacked");
  if (!costume_from_download(fetched.bytes, slot, scratch.u8string(), &dat, &why)) { r.summary = why; return r; }
  const ServedForm served = costume_served_form(retail, dat, slot, peer_skin_policy());
  if (!served.ok) { r.summary = "served form: " + served.reason; return r; }
  r.ok = true;
  r.bytes = std::make_shared<const std::vector<uint8_t>>(served.bytes);
  r.source = std::make_shared<const std::vector<uint8_t>>(std::move(dat));
  r.summary = std::string(fetched.from_cache ? "cached" : "fetched") + ", made for " +
              (served.source_slot.empty() ? std::string("?") : served.source_slot) + ", " + served.reason;
  return r;
}

void finish(const Job& job, PortSkin result) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_generation[job.port] != job.generation) return;   // a newer pick, or reset(): thrown away
  // A ready skin's stock icon is fetched next, behind any port's skin still queued.
  if (result.state == PortState::kReady && result.stock_state == PortState::kWorking) {
    Job stock = job;
    stock.stock = true;
    g_jobs.push_back(stock);
  }
  g_ports[job.port] = std::move(result);
  g_changed.notify_all();
}

// The ready skin's own stock icon (skin_fetch.h fetch_stock_icon), as the HUD's 24x24 texture. Never
// changes the skin: whatever happens here, the port keeps it, and only the icon is the retail one.
void run_stock(const Job& job) {
  const auto start = std::chrono::steady_clock::now();
  PortState state = PortState::kRefused;
  Bytes pixels;
  std::string why;
  try {
    const StockIcon icon = fetch_stock_icon(job.record.file.mod_id, job.record.file.file_id, wire::hex(job.record.file.sha256));
    why = icon.reason;
    if (icon.ok) {
      std::vector<uint8_t> texture = host::roster::console_picture(icon.path, kStockSide, kStockSide);
      if (texture.size() == (size_t)kStockSide * kStockSide * 4) {
        state = PortState::kReady;
        pixels = std::make_shared<const std::vector<uint8_t>>(std::move(texture));
      } else {
        why = "its stock icon could not be read (" + icon.reason + ")";
      }
    } else if (icon.none) {
      state = PortState::kNone;
    }
  } catch (const std::exception& e) {
    state = PortState::kRefused;
    why = std::string("an error while reading its stock icon (") + e.what() + ")";
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    PortSkin& p = g_ports[job.port];
    if (g_generation[job.port] != job.generation || p.state != PortState::kReady) return;   // a newer pick
    p.stock_state = state;
    p.stock = pixels;
  }
  const long long ms =
      (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  host::log("opponent skins: port %d: mod %u file %u: its stock icon %s in %lld ms: %s", job.port, job.record.file.mod_id,
            job.record.file.file_id,
            state == PortState::kReady ? "ready" : state == PortState::kNone ? "none (the retail icon)" : "refused (the retail icon)",
            ms, why.c_str());
}

void run(const Job& job) {
  const auto start = std::chrono::steady_clock::now();
  PortSkin out;
  out.character = job.character;
  out.costume = job.costume;
  out.generation = job.generation;
  out.slot = host::cosmetics::costume_slot_file(job.character, job.costume);
  out.partner_slot = job.record.has_partner ? partner_slot(job.character, job.costume) : std::string();
  const auto refuse = [&](const std::string& why) {
    out.state = PortState::kRefused;
    out.reason = why;
    host::log("opponent skins: port %d: mod %u file %u for %s refused: %s", job.port, job.record.file.mod_id,
              job.record.file.file_id, out.slot.empty() ? "?" : out.slot.c_str(), why.c_str());
    finish(job, out);
  };
  if (job.record.character != job.character || job.record.costume != job.costume) {
    refuse("named character " + std::to_string(job.record.character) + " costume " + std::to_string(job.record.costume) +
           ", the pick is character " + std::to_string(job.character) + " costume " + std::to_string(job.costume));
    return;
  }
  if (out.slot.empty()) { refuse("the game has no such costume"); return; }
  const FileResult main = serve_one(job.record.file, out.slot);
  if (!main.ok) { refuse(main.summary); return; }
  out.state = PortState::kReady;
  out.stock_state = PortState::kWorking;   // its stock icon next (finish queues it)
  out.bytes = main.bytes;
  out.source = main.source;
  out.reason = main.summary;
  if (job.record.has_partner) {
    const FileResult partner = out.partner_slot.empty() ? FileResult{false, nullptr, "no partner slot", nullptr}
                                                        : serve_one(job.record.partner, out.partner_slot);
    if (partner.ok) {
      out.partner_bytes = partner.bytes;
      out.partner_source = partner.source;
    }
    host::log("opponent skins: port %d: partner %s: %s%s", job.port, out.partner_slot.c_str(),
              partner.ok ? "ready, " : "refused (its retail costume is served): ", partner.summary.c_str());
  }
  const long long ms =
      (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  host::log("opponent skins: port %d: mod %u file %u ready for %s in %lld ms (%zu bytes): %s", job.port,
            job.record.file.mod_id, job.record.file.file_id, out.slot.c_str(), ms, out.bytes->size(), out.reason.c_str());
  finish(job, out);
}

void worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_changed.wait(lock, [] { return !g_jobs.empty(); });
      job = g_jobs.front();
      g_jobs.pop_front();
      if (g_generation[job.port] != job.generation) continue;   // replaced before it started
    }
    if (job.stock) {   // catches its own: a picture never takes the skin away
      run_stock(job);
      continue;
    }
    // Nothing a download holds may end the game: anything thrown is that port's refusal (its retail
    // costume), as any other failed step is.
    try {
      run(job);
    } catch (const std::exception& e) {
      PortSkin out;
      out.state = PortState::kRefused;
      out.character = job.character;
      out.costume = job.costume;
      out.generation = job.generation;
      out.slot = host::cosmetics::costume_slot_file(job.character, job.costume);
      out.reason = std::string("an error while reading it (") + e.what() + ")";
      host::log("opponent skins: port %d: mod %u file %u refused: %s", job.port, job.record.file.mod_id,
                job.record.file.file_id, out.reason.c_str());
      finish(job, out);
    }
  }
}

// The disc offset of a costume slot's own file, by which the cosmetics know the skin they serve for
// it; 0 when the open disc has no such file.
uint32_t slot_start(const std::string& slot) {
  uint32_t offset = 0, size = 0;
  return host::disc_find_file(slot, &offset, &size) && size ? offset : 0;
}

// The skin this machine serves for `slot` itself (a catalog skin the player picked), or "".
std::string own_asset(const std::string& slot) {
  const uint32_t start = slot_start(slot);
  return start ? host::cosmetics::applied_asset(start) : std::string();
}

// A file this machine builds from what it has itself, once per `key`: the retail file of `slot`
// (`source` null), or the served form of the costume `source` gives, by the rule an opponent's machine
// applies to it (peer_skin_policy) unless `policy` says otherwise. Null when it cannot be had.
Bytes built(const std::string& key, const std::string& slot, int port,
            bool (*source)(const std::string& what, const std::string& slot, std::vector<uint8_t>* dat, std::string* why),
            const std::string& what, const ServedFormPolicy& policy = peer_skin_policy()) {
  {
    std::lock_guard<std::mutex> lock(g_served_mutex);
    const auto it = g_built.find(key);
    if (it != g_built.end()) return it->second;
  }
  std::vector<uint8_t> retail, dat;
  std::string why;
  Bytes out;
  if (!retail_reference(slot, &retail, &why)) {
    host::log("opponent skins: port %d: %s for %s: no retail reference (%s)", port, source ? what.c_str() : "retail file",
              slot.c_str(), why.c_str());
  } else if (!source) {
    out = std::make_shared<const std::vector<uint8_t>>(std::move(retail));
  } else if (!source(what, slot, &dat, &why)) {
    host::log("opponent skins: port %d: %s for %s refused: %s", port, what.c_str(), slot.c_str(), why.c_str());
  } else {
    ServedForm served = costume_served_form(retail, dat, slot, policy);
    if (served.ok) {
      host::log("opponent skins: port %d: %s ready for %s (%zu bytes): %s", port, what.c_str(), slot.c_str(),
                served.bytes.size(), served.reason.c_str());
      out = std::make_shared<const std::vector<uint8_t>>(std::move(served.bytes));
    } else {
      host::log("opponent skins: port %d: %s for %s refused: served form: %s", port, what.c_str(), slot.c_str(),
                served.reason.c_str());
    }
  }
  std::lock_guard<std::mutex> lock(g_served_mutex);
  return g_built.emplace(key, out).first->second;
}

// An opponent's ready skin in another costume of its fighter than the one picked (Teams plays each
// player in their team's color, after the pick that named the skin): the served form of the same
// costume file for `slot`, built against `slot`'s own retail file, so its skeleton and rest pose are
// that slot's (peer_skin_policy, as for the slot picked). The sender's machine wears its own skin the
// same way (costume_slots.cpp served_for). Built once per pick and slot; null when `slot` is no
// costume of that fighter (or the partner's), or the skin does not pass for it.
Bytes recolored(int port, const PortSkin& remote, const std::string& slot) {
  bool partner = false;
  if (remote.state != PortState::kReady ||
      costume_of_slot((uint8_t)remote.character, slot, remote.partner_source != nullptr, &partner) < 0)
    return nullptr;
  const std::string low = lower(slot);
  {
    std::lock_guard<std::mutex> lock(g_served_mutex);
    if (g_recolor_generation[port] != remote.generation) {
      g_recolored[port].clear();
      g_recolor_generation[port] = remote.generation;
    }
    const auto it = g_recolored[port].find(low);
    if (it != g_recolored[port].end()) return it->second;
  }
  std::vector<uint8_t> retail;
  std::string why;
  Bytes out;
  const char* made_for = partner ? remote.partner_slot.c_str() : remote.slot.c_str();
  if (!retail_reference(slot, &retail, &why)) {
    host::log("opponent skins: port %d: the skin for %s in %s (the color played): no retail reference (%s); its retail costume",
              port, made_for, slot.c_str(), why.c_str());
  } else {
    try {
      out = recolor_form(remote, slot, retail, &why);
    } catch (const std::exception& e) {   // the game's thread: a refusal, never the end of the game
      out = nullptr;
      why = std::string("an error while building it (") + e.what() + ")";
    }
    if (out)
      host::log("opponent skins: port %d: the skin for %s ready in %s, the color played (%zu bytes): %s", port, made_for,
                slot.c_str(), out->size(), why.c_str());
    else
      host::log("opponent skins: port %d: the skin for %s refused in %s, the color played: %s; its retail costume", port,
                made_for, slot.c_str(), why.c_str());
  }
  std::lock_guard<std::mutex> lock(g_served_mutex);
  if (g_recolor_generation[port] != remote.generation) return out;   // a newer pick meanwhile: not kept
  return g_recolored[port].emplace(low, out).first->second;
}

// A skin file on this machine (the test hook): the costume for `slot` out of it.
bool file_source(const std::string& path, const std::string& slot, std::vector<uint8_t>* dat, std::string* why) {
  std::vector<uint8_t> download;
  if (!read_file(fs::u8path(path), kMaxSkinBytes * 4, &download)) { *why = "cannot read " + path; return false; }
  const fs::path scratch = fs::u8path(cache_folder()) / L"port-test.unpacked";
  return costume_from_download(download, slot, scratch.u8string(), dat, why);
}
// A catalog skin of this machine: its file as the game would load it.
bool asset_source(const std::string& asset_id, const std::string&, std::vector<uint8_t>* dat, std::string* why) {
  std::string thumbnail;
  if (!host::cosmetics::costume_file(asset_id, dat, &thumbnail) || dat->empty()) { *why = "the skin's file cannot be read"; return false; }
  return true;
}

// MELEE_TEST_PORT_SKIN's file for `port`, or "".
std::string test_port_file(int port) {
  const char* hook = host::test_env::automated_getenv("MELEE_TEST_PORT_SKIN");
  if (!hook || !*hook) return {};
  const std::string spec = hook;
  size_t from = 0;
  while (from <= spec.size()) {
    size_t end = spec.find(';', from);
    if (end == std::string::npos) end = spec.size();
    const std::string item = spec.substr(from, end - from);
    if (item.size() > 2 && item[1] == ':' && item[0] - '0' == port) return item.substr(2);
    from = end + 1;
  }
  return {};
}

// <settings dir>/CosmeticMods/nucleus-origins.json (record_origin).
std::mutex g_origin_mutex;
fs::path origins_path() {
  const fs::path cache = fs::u8path(cache_folder());
  return cache.empty() ? fs::path() : cache.parent_path() / L"CosmeticMods" / L"nucleus-origins.json";
}
nlohmann::json read_origins() {
  std::vector<uint8_t> bytes;
  const fs::path path = origins_path();
  if (path.empty() || !read_file(path, 4u << 20, &bytes)) return nlohmann::json::object();
  nlohmann::json doc = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
  return doc.is_object() ? doc : nlohmann::json::object();
}
}  // namespace

void set_enabled(bool on) {
  if (g_enabled.exchange(on) != on) host::log("opponent skins: %s", on ? "on" : "off");
  if (!on) reset();
}
bool enabled() { return g_enabled.load(); }

const char* state_name(PortState state) {
  switch (state) {
    case PortState::kNone: return "none";
    case PortState::kWorking: return "working";
    case PortState::kReady: return "ready";
    case PortState::kRefused: return "refused";
  }
  return "?";
}

PortSkin port_skin(int port) {
  if (port < 0 || port >= kPorts) return {};
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_ports[port];
}

PortState wait_port(int port, int timeout_ms) {
  if (port < 0 || port >= kPorts) return PortState::kNone;
  std::unique_lock<std::mutex> lock(g_mutex);
  g_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms),
                     [port] { return g_ports[port].state != PortState::kWorking; });
  return g_ports[port].state;
}

PortState worn_stock(int port, Bytes* pixels) {
  pixels->reset();
  if (port < 0 || port >= kPorts) return PortState::kNone;
  uint64_t worn = 0;
  {
    std::lock_guard<std::mutex> lock(g_served_mutex);
    worn = g_worn_generation[port];
  }
  if (!worn) return PortState::kNone;
  std::lock_guard<std::mutex> lock(g_mutex);
  const PortSkin& p = g_ports[port];
  if (p.state != PortState::kReady || p.generation != worn) return PortState::kNone;
  if (p.stock_state == PortState::kReady && p.stock) {
    *pixels = p.stock;
    return PortState::kReady;
  }
  return p.stock_state == PortState::kWorking ? PortState::kWorking : PortState::kNone;
}

void opponent_selected(int port, uint8_t character, uint8_t costume, const std::vector<wire::Record>& records) {
  if (port < 0 || port >= kPorts || !enabled()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t generation = ++g_generation[port];
  const bool had = g_ports[port].state != PortState::kNone;
  g_ports[port] = PortSkin{};
  if (records.empty()) {
    if (had) host::log("opponent skins: port %d: no skin for this pick (retail costume)", port);
    g_changed.notify_all();
    return;
  }
  // slippi_net binds every record to the sender's port, so there is one.
  Job job{port, generation, character, costume, records.front()};
  g_ports[port].state = PortState::kWorking;
  g_ports[port].character = character;
  g_ports[port].costume = costume;
  g_jobs.push_back(job);
  host::log("opponent skins: port %d: wants mod %u file %u (sha256 %.12s...)%s for character %u costume %u", port,
            job.record.file.mod_id, job.record.file.file_id, wire::hex(job.record.file.sha256).c_str(),
            job.record.has_partner ? " with a partner file" : "", character, costume);
  if (!g_worker) {
    g_worker = true;
    std::thread(worker).detach();
  }
  g_changed.notify_all();
}

void reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (int p = 0; p < kPorts; ++p) {
    ++g_generation[p];
    g_ports[p] = PortSkin{};
  }
  g_jobs.clear();
  g_changed.notify_all();
  g_local_port = -1;
  std::lock_guard<std::mutex> served(g_served_mutex);
  for (auto& answer : g_answer) answer = Answer{};
  g_built.clear();
  for (auto& r : g_recolored) r.clear();
  for (auto& w : g_worn_generation) w = 0;
}

bool local_payload(uint8_t port, uint8_t character, uint8_t costume, std::vector<uint8_t>* payload) {
  payload->clear();
  // The local port is kept with the setting off too: the player's own costume slot is shown to them
  // whatever the setting says (served_costume).
  if (port < kPorts) g_local_port = port;
  if (!enabled()) return false;
  const char* hook = host::test_env::automated_getenv("MELEE_TEST_SEND_SKIN");
  if (!hook || !*hook) {
    // The player's own skin for this pick, when the mod browser recorded its SSBM Nucleus ids and its
    // served form passes here (what the opponent's machine will build from the same file): the skin
    // picked as a costume slot on the select screen, else (a mod disc's skin picked with L / R) the
    // one this machine serves for the pick's costume file. The message names the skin by its ids;
    // the pick itself (0x82) carries the retail costume either way.
    const std::string slot = host::cosmetics::costume_slot_file(character, costume);
    std::string slot_partner;
    const std::string picked = slot.empty() ? std::string() : host::costume_slots::online_announce(character, costume, &slot_partner);
    const std::string asset = !picked.empty() ? picked : slot.empty() ? std::string() : own_asset(slot);
    wire::Record r;
    if (asset.empty() || !origin_of(asset, &r.file)) return false;
    if (picked.empty() && !built("asset:" + asset + "|" + slot, slot, port, asset_source, asset)) {
      host::log("opponent skins: own skin %s for %s does not pass; nothing sent", asset.c_str(), slot.c_str());
      return false;
    }
    r.port = port;
    r.character = character;
    r.costume = costume;
    if (wire::two_file_character(character)) {
      const std::string other = partner_slot(character, costume);
      const std::string partner = other.empty() ? std::string() : !picked.empty() ? slot_partner : own_asset(other);
      r.has_partner = !partner.empty() && origin_of(partner, &r.partner) &&
                      (!picked.empty() || built("asset:" + partner + "|" + other, other, port, asset_source, partner) != nullptr);
      if (!r.has_partner) r.partner = wire::FileRef{};
    }
    wire::Message m;
    m.sender_port = port;
    m.records.push_back(r);
    *payload = wire::encode(m);
    if (payload->empty()) return false;
    host::log("opponent skins: own skin %s for %s: mod %u file %u%s", asset.c_str(), slot.c_str(), r.file.mod_id,
              r.file.file_id, r.has_partner ? " with its partner file" : "");
    return true;
  }
  const std::string spec = hook;
  if (spec == "malformed") {   // a record count with no record after it
    *payload = {'M', 'U', 'S', wire::kVersion, port, 1};
    return true;
  }
  wire::Message m;
  m.sender_port = port;
  wire::Record r;
  r.port = spec == "otherport" ? (uint8_t)((port + 1) % 4) : port;
  r.character = character;
  r.costume = costume;
  if (spec == "otherport") {
    r.file.mod_id = 1; r.file.file_id = 1; r.file.sha256.fill(0x11);
  } else {
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t at; (at = spec.find(':', from)) != std::string::npos; from = at + 1) parts.push_back(spec.substr(from, at - from));
    parts.push_back(spec.substr(from));
    const auto file_ref = [&](size_t i, wire::FileRef* f) {
      f->mod_id = (uint32_t)std::strtoul(parts[i].c_str(), nullptr, 10);
      f->file_id = (uint32_t)std::strtoul(parts[i + 1].c_str(), nullptr, 10);
      return wire::from_hex(parts[i + 2], &f->sha256);
    };
    if ((parts.size() != 3 && parts.size() != 6) || !file_ref(0, &r.file) || (parts.size() == 6 && !file_ref(3, &r.partner))) {
      host::log("opponent skins: MELEE_TEST_SEND_SKIN not understood, nothing sent");
      return false;
    }
    r.has_partner = parts.size() == 6;
  }
  m.records.push_back(r);
  *payload = wire::encode(m);
  if (payload->empty()) {
    host::log("opponent skins: the test skin does not fit this pick (character %u costume %u), nothing sent", character, costume);
    return false;
  }
  return true;
}

std::string partner_slot(uint8_t character, uint8_t costume) {
  if (character == 14) {   // Nana: her colours in the costume order (cosmetic_mods_internal.h families[])
    static const char* const nana[] = {"Nr", "Ye", "Aq", "Wh"};
    return costume < 4 ? std::string("PlNn") + nana[costume] + ".dat" : std::string();
  }
  if (character == 18) return host::cosmetics::costume_slot_file(19, costume);
  if (character == 19) return host::cosmetics::costume_slot_file(18, costume);
  return {};
}

int costume_of_slot(uint8_t character, const std::string& slot, bool with_partner, bool* partner) {
  *partner = false;
  const std::string low = lower(slot);
  if (low.empty()) return -1;
  for (int k = 0; k < 8; ++k) {
    const std::string own = host::cosmetics::costume_slot_file(character, k);
    if (!own.empty() && lower(own) == low) return k;
  }
  if (!with_partner) return -1;
  for (int k = 0; k < 8; ++k) {
    const std::string other = partner_slot(character, (uint8_t)k);
    if (!other.empty() && lower(other) == low) {
      *partner = true;
      return k;
    }
  }
  return -1;
}

std::shared_ptr<const std::vector<uint8_t>> recolor_form(const PortSkin& remote, const std::string& slot,
                                                         const std::vector<uint8_t>& retail_slot_bytes, std::string* why) {
  bool partner = false;
  if (remote.state != PortState::kReady) {
    *why = "no skin ready for this port";
    return nullptr;
  }
  if (costume_of_slot((uint8_t)remote.character, slot, remote.partner_source != nullptr, &partner) < 0) {
    *why = slot + " is no costume of this pick's fighter";
    return nullptr;
  }
  const auto& source = partner ? remote.partner_source : remote.source;
  if (!source) {
    *why = "no costume file kept for it";
    return nullptr;
  }
  ServedForm served = costume_served_form(retail_slot_bytes, *source, slot, peer_skin_policy());   // as for the slot picked
  if (!served.ok) {
    *why = "served form: " + served.reason;
    return nullptr;
  }
  *why = served.reason;
  return std::make_shared<const std::vector<uint8_t>>(std::move(served.bytes));
}

bool retail_reference(const std::string& slot, std::vector<uint8_t>* bytes, std::string* why) {
  const RetailCostume* known = nullptr;
  for (const auto& c : kRetailCostumes)
    if (lower(c.file) == lower(slot)) known = &c;
  if (!known) { *why = slot + " is not a retail costume"; return false; }
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(known->file, &offset, &size)) { *why = "the open disc has no " + slot; return false; }
  if (size != known->size) { *why = "the open disc's " + slot + " is not the retail 1.02 file (size)"; return false; }
  bytes->resize(size);
  if (!host::disc_read(offset, bytes->data(), size)) { *why = "the open disc's " + slot + " could not be read"; return false; }
  if (host::hackpack_ai::rules::sha256_hex(bytes->data(), bytes->size()) != known->sha256) {
    *why = "the open disc's " + slot + " is not the retail 1.02 file (hash)";
    return false;
  }
  return true;
}

bool costume_from_download(const std::vector<uint8_t>& download, const std::string& slot, const std::string& scratch_dir,
                           std::vector<uint8_t>* dat, std::string* why) {
  const bool zip = download.size() >= 4 && std::memcmp(download.data(), "PK\x03\x04", 4) == 0;
  if (!zip) {
    *dat = download;   // one DAT; costume_served_form judges it
    return true;
  }
  (void)scratch_dir;   // read in memory now (archive_unpack.h); nothing is unpacked to disk
  // A skin post is a handful of files: the costume, maybe its partner, pictures, a read-me.
  host::archive::Zip archive;
  std::string bad;
  if (!archive.open_memory(download, {64, 64ull << 20, 64ull << 20, 250}, &bad)) {
    *why = "the download is not a zip this build unpacks (" + bad + ")";
    return false;
  }
  const std::string fighter = lower(slot.substr(0, 4));   // "plfx"
  std::vector<std::pair<std::string, std::vector<uint8_t>>> costumes;   // (slot it is for, bytes)
  int files = 0;
  for (const host::archive::Entry& e : archive.entries()) {
    if (e.folder()) continue;
    const std::string ext = e.extension();   // from the name's bytes: a legacy-encoded name must not throw
    if (ext != ".dat" && ext != ".usd") continue;
    ++files;
    std::vector<uint8_t> bytes;
    if (e.size > kMaxCostumeBytes || !archive.read(e, &bytes, &bad)) continue;
    const auto identity = host::cosmetics::testing::inspect_dat(bytes);
    if (identity.ok && lower(identity.target_path).rfind(fighter, 0) == 0) costumes.emplace_back(lower(identity.target_path), std::move(bytes));
  }
  int exact = -1, matches = 0;
  for (size_t i = 0; i < costumes.size(); ++i)
    if (costumes[i].first == lower(slot)) { exact = (int)i; ++matches; }
  if (matches == 1) { *dat = std::move(costumes[(size_t)exact].second); return true; }
  if (matches == 0 && costumes.size() == 1) { *dat = std::move(costumes.front().second); return true; }
  *why = costumes.empty() ? "no costume of this fighter in the download (" + std::to_string(files) + " game files)"
                          : std::to_string(costumes.size()) + " costumes of this fighter in the download, " +
                                (matches ? std::to_string(matches) + " for " + slot : "none for " + slot);
  return false;
}

bool served_costume(int port, const std::string& slot, bool online, Bytes* bytes) {
  bytes->reset();
  if (port < 0 || port >= kPorts || slot.empty()) return false;
  const std::string low = lower(slot);
  Bytes out;
  const char* why = "";
  std::string slot_asset;
  // What the port's fighter wears from an opponent's skin (worn_stock): that pick's generation, 0 none;
  // a partner file (Nana's, Sheik's) leaves it as it is.
  uint64_t wears = 0;
  bool partner_file = false;
  // The port's own costume slot (an installed skin picked on the select screen), with the setting on
  // or off: offline every human port's pick, online the local player's.
  out = host::costume_slots::served_for(port, slot, online, g_local_port.load(), &slot_asset);
  if (out) why = "its costume slot";
  if (out || !enabled()) {
    // (the setting off: nothing else is served)
  } else if (const std::string file = test_port_file(port); !file.empty()) {
    out = built("test:" + file + "|" + low, slot, port, file_source, file);
    why = "the test skin";
  } else if (online) {
    const PortSkin remote = port_skin(port);
    if (remote.state == PortState::kReady) {
      bool partner = false;
      partner_file = costume_of_slot((uint8_t)remote.character, slot, true, &partner) >= 0 && partner;
    }
    if (remote.state == PortState::kReady && lower(remote.slot) == low) {
      out = remote.bytes;
      why = "the opponent's skin";
    } else if (remote.state == PortState::kReady && !remote.partner_slot.empty() && lower(remote.partner_slot) == low &&
               remote.partner_bytes) {
      out = remote.partner_bytes;
      why = "the opponent's skin (partner file)";
    } else if (remote.state == PortState::kReady) {
      out = recolored(port, remote, slot);   // a Teams color: the same skin, that slot's served form
      if (out) why = "the opponent's skin in the color played";
    }
    if (out && !partner_file) wears = remote.generation;
    const std::string asset = out ? std::string() : own_asset(slot);
    if (!out && !asset.empty() && port == g_local_port.load()) {
      // The player's own screen: the peer rule (own_skin_policy), so a skin an opponent's machine would
      // refuse is the retail costume here too.
      out = built("own:" + asset + "|" + low, slot, port, asset_source, asset, own_skin_policy());
      why = "the player's own skin";
    }
    if (!out && !asset.empty()) {
      // This machine serves its own skin for the slot; another port (or the player's own, when the
      // skin does not pass) shows the retail costume, as the opponent's machine shows it.
      out = built("retail|" + low, slot, port, nullptr, "");
      why = "the retail costume (this machine's own skin stays on its own port)";
    }
  }
  std::lock_guard<std::mutex> lock(g_served_mutex);
  if (!partner_file) g_worn_generation[port] = wears;
  Answer& answer = g_answer[port];
  if (out && (lower(answer.slot) != low || answer.bytes != out))
    host::log("opponent skins: port %d loads %s%s%s for %s (%zu bytes)", port, why, slot_asset.empty() ? "" : " ",
              slot_asset.c_str(), slot.c_str(), out->size());
  answer.slot = out ? slot : std::string();
  answer.bytes = out;
  *bytes = out;
  return out != nullptr;
}

void record_origin(const std::string& asset_id, uint32_t mod_id, uint32_t file_id, const std::string& downloaded_path,
                   const std::string& archive_sha256) {
  if (asset_id.empty() || !mod_id || !file_id) return;
  std::vector<uint8_t> bytes;
  if (!read_file(fs::u8path(downloaded_path), 96u << 20, &bytes)) return;
  const std::string sha = host::hackpack_ai::rules::sha256_hex(bytes.data(), bytes.size());
  std::lock_guard<std::mutex> lock(g_origin_mutex);
  const fs::path path = origins_path();
  if (path.empty()) return;
  nlohmann::json doc = read_origins();
  doc[asset_id] = {{"mod", mod_id}, {"file", file_id}, {"sha256", sha}};
  if (archive_sha256.size() == 64) doc[asset_id]["archive_sha256"] = archive_sha256;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path staged = fs::path(path).concat(L".tmp");
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out << doc.dump(2) << "\n";
    if (!out) { fs::remove(staged, ec); return; }
  }
  fs::rename(staged, path, ec);
  if (ec) { fs::remove(staged, ec); return; }
  host::log("opponent skins: %s is SSBM Nucleus mod %u file %u (sha256 %.12s...)", asset_id.c_str(), mod_id, file_id, sha.c_str());
}

bool origin_of(const std::string& asset_id, wire::FileRef* out) {
  std::lock_guard<std::mutex> lock(g_origin_mutex);
  const nlohmann::json doc = read_origins();
  const auto it = doc.find(asset_id);
  if (it == doc.end() || !it->is_object()) return false;
  const auto& o = *it;
  if (!o.contains("mod") || !o["mod"].is_number_unsigned() || !o.contains("file") || !o["file"].is_number_unsigned() ||
      !o.contains("sha256") || !o["sha256"].is_string())
    return false;
  const uint64_t mod = o["mod"].get<uint64_t>(), file = o["file"].get<uint64_t>();
  if (!mod || !file || mod > UINT32_MAX || file > UINT32_MAX || !wire::from_hex(o["sha256"].get<std::string>(), &out->sha256))
    return false;
  out->mod_id = (uint32_t)mod;
  out->file_id = (uint32_t)file;
  return true;
}

}  // namespace host::skins
