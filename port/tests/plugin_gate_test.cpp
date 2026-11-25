// Fighter plugin approval (app/plugin_gate.h): SHA-256, the detached Ed25519 signature, the approvals
// file, the plugin record's fighter name, the gate that copies only signed or approved plugins for the
// game library, and the command line. Signs with the TEST-ONLY key in tests/plugin_test_key.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_gate.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "monocypher-ed25519.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace plugin_gate;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

static std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static std::string sha_of(const std::string& s) { return sha256_hex((const uint8_t*)s.data(), s.size()); }

static void write_file(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write((const char*)data.data(), (std::streamsize)data.size());
}

static std::vector<uint8_t> read_all(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The hex line of a TEST_ONLY key file (lines starting with # are comments).
static std::vector<uint8_t> key_file(const fs::path& path) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < line.size(); i += 2) out.push_back((uint8_t)std::stoul(line.substr(i, 2), nullptr, 16));
    return out;
  }
  return {};
}

// ---- a small plugin object, as tools/mexft/mexft_plugin.py lays one out ----
static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = (uint8_t)v; b[at + 1] = (uint8_t)(v >> 8); }
static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }
static void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }

// One .rdata section: the MuFighterPlugin record at 0, its one entry at 40, the descriptor at 56, then
// the strings. `with_mex_name`: the entry names the fighter; else only the descriptor does. `salt`
// changes the bytes (another plugin file).
static std::vector<uint8_t> make_plugin(bool with_mex_name, uint8_t salt = 0) {
  std::vector<uint8_t> data(112, 0);
  put32(data, 0, 0x5446554Du);      // magic "MUFT"
  put32(data, 4, 2);                // version
  data[8] = salt;                   // abi[0]
  put64(data, 24, 40);              // fighters -> entry (addend)
  put32(data, 32, 1);               // fighter_count
  put64(data, 40, 56);              // entry.descriptor -> descriptor
  put64(data, 48, 72);              // entry.mex_name -> "Wolf"
  put64(data, 56, 86);              // descriptor.name -> "Wolfy"
  std::memcpy(&data[72], "Wolf\0PlWf.dat\0Wolfy\0", 20);
  struct Reloc { uint32_t at; };
  std::vector<Reloc> relocs = {{24}, {40}, {56}};
  if (with_mex_name) relocs.push_back({48});
  const size_t data_at = 20 + 40, reloc_at = data_at + data.size(), sym_at = reloc_at + relocs.size() * 10;
  const char* name = "mu_fighter_plugin";
  std::vector<uint8_t> b(sym_at + 2 * 18 + 4 + std::strlen(name) + 1, 0);
  put16(b, 0, 0x8664);
  put16(b, 2, 1);
  put32(b, 8, (uint32_t)sym_at);
  put32(b, 12, 2);
  std::memcpy(&b[20], ".rdata", 6);
  put32(b, 20 + 16, (uint32_t)data.size());
  put32(b, 20 + 20, (uint32_t)data_at);
  put32(b, 20 + 24, (uint32_t)reloc_at);
  put16(b, 20 + 32, (uint16_t)relocs.size());
  put32(b, 20 + 36, 0x40000040u);
  std::memcpy(&b[data_at], data.data(), data.size());
  for (size_t i = 0; i < relocs.size(); ++i) {
    put32(b, reloc_at + i * 10, relocs[i].at);
    put32(b, reloc_at + i * 10 + 4, 0);   // symbol 0: the section
    put16(b, reloc_at + i * 10 + 8, 1);   // IMAGE_REL_AMD64_ADDR64
  }
  std::memcpy(&b[sym_at], ".rdata", 6);
  put16(b, sym_at + 12, 1);
  b[sym_at + 16] = 3;                      // static
  put32(b, sym_at + 18 + 4, 4);            // long name at string table offset 4
  put16(b, sym_at + 18 + 12, 1);
  b[sym_at + 18 + 16] = 2;                 // external
  put32(b, sym_at + 36, (uint32_t)(4 + std::strlen(name) + 1));
  std::memcpy(&b[sym_at + 40], name, std::strlen(name));
  return b;
}

static int run_cli(std::vector<std::string> args, const fs::path& program_dir, json* out) {
  args.insert(args.begin(), "melee_source.exe");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  std::string text;
  const int code = cli_run((int)argv.size(), argv.data(), program_dir, &text);
  *out = json::parse(text, nullptr, false);
  return code;
}

static void test_sha256() {
  CHECK(sha_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha_of(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(is_sha256_hex(sha_of("x")) && !is_sha256_hex("abc") && !is_sha256_hex(std::string(64, 'g')));
}

struct TestKey {
  uint8_t secret[64];
  Key public_key;
};

static TestKey test_key() {
  TestKey k{};
  std::vector<uint8_t> seed = key_file(MELEE_PLUGIN_TEST_SEED);
  CHECK(seed.size() == 32);
  seed.resize(32);
  crypto_ed25519_key_pair(k.secret, k.public_key.data(), seed.data());   // wipes the seed
  fs::path pub = fs::path(MELEE_PLUGIN_TEST_SEED).parent_path() / "TEST_ONLY_ed25519_public.txt";
  const std::vector<uint8_t> expected = key_file(pub);
  CHECK(expected.size() == 32 && std::memcmp(expected.data(), k.public_key.data(), 32) == 0);
  return k;
}

static std::vector<uint8_t> sign(const TestKey& k, const std::vector<uint8_t>& file) {
  uint8_t digest[32];
  sha256(file.data(), file.size(), digest);
  const std::vector<uint8_t> message = signed_message(digest);
  std::vector<uint8_t> sig(64);
  crypto_ed25519_sign(sig.data(), k.secret, message.data(), message.size());
  return sig;
}

static void test_signatures(const TestKey& k) {
  const std::vector<uint8_t> file = make_plugin(true);
  uint8_t digest[32];
  sha256(file.data(), file.size(), digest);
  const std::vector<uint8_t> raw = sign(k, file);
  const std::string hex_text = "  " + hex(raw.data(), raw.size()) + "\r\n";
  const std::vector<Key> keys = {k.public_key};
  CHECK(check_signature(digest, raw, keys) == Signature::Valid);
  CHECK(check_signature(digest, bytes_of(hex_text), keys) == Signature::Valid);
  CHECK(check_signature(digest, {}, keys) == Signature::None);
  std::vector<uint8_t> bad = raw;
  bad[10] ^= 1;
  CHECK(check_signature(digest, bad, keys) == Signature::Invalid);
  CHECK(check_signature(digest, bytes_of(std::string(128, 'z')), keys) == Signature::Invalid);
  CHECK(check_signature(digest, bytes_of("1234"), keys) == Signature::Invalid);
  uint8_t other[32];
  std::memcpy(other, digest, 32);
  other[0] ^= 0x80;
  CHECK(check_signature(other, raw, keys) == Signature::Invalid);   // one changed byte of the file
  CHECK(check_signature(digest, raw, {}) == Signature::Invalid);     // no key: nothing verifies
  CHECK(check_signature(digest, raw, {Key{}}) == Signature::Invalid);   // the empty production slot
  // The signed message is "MUFTSIG1" + the digest.
  const std::vector<uint8_t> message = signed_message(digest);
  CHECK(message.size() == 40 && std::memcmp(message.data(), "MUFTSIG1", 8) == 0 && std::memcmp(&message[8], digest, 32) == 0);
  // The built-in list: empty in every build but a test-key one, which trusts only the test key.
  if (builtin_test_key()) CHECK(builtin_keys().size() == 1 && builtin_keys()[0] == k.public_key);
  else CHECK(builtin_keys().empty());
}

static void test_name() {
  CHECK(fighter_name(make_plugin(true)) == "Wolf");
  CHECK(fighter_name(make_plugin(false)) == "Wolfy");
  CHECK(looks_like_plugin(make_plugin(true)));
  CHECK(!looks_like_plugin(bytes_of("not a plugin, not even close to sixty four bytes long, no no no no no")));
  // Untrusted bytes: every truncation and every single flipped byte answers without reading outside.
  const std::vector<uint8_t> good = make_plugin(true);
  for (size_t n = 0; n < good.size(); ++n) fighter_name(std::vector<uint8_t>(good.begin(), good.begin() + n));
  for (size_t i = 0; i < good.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> v = good;
      v[i] ^= (uint8_t)(1 << bit);
      const std::string name = fighter_name(v);
      CHECK(name.size() <= 64);
    }
}

static void test_store(const fs::path& dir) {
  const fs::path file = dir / "plugin-approvals.json";
  Store s = load_store(file);
  CHECK(s.entries.empty() && s.error.empty());
  std::string error;
  const std::string sha = sha_of("one");
  CHECK(update_store(file, [&](Store& st) { st.entries[sha] = {true, "Wolf", "wolf.mufighter", now_utc()}; }, &error));
  s = load_store(file);
  CHECK(s.entries.size() == 1 && s.entries[sha].approved && s.entries[sha].name == "Wolf");
  const json doc = json::parse(read_all(file), nullptr, false);
  CHECK(doc.is_object() && doc["format"] == 1 && doc["plugins"][sha]["approved"] == true &&
        doc["plugins"][sha]["file"] == "wolf.mufighter" && doc["plugins"][sha]["date"].is_string());
  CHECK(!fs::exists(dir / "plugin-approvals.json.lock") && !fs::exists(dir / "plugin-approvals.json.part"));
  // An unreadable file approves nothing, and is kept aside when the store is next written.
  write_file(file, bytes_of("{ not json"));
  s = load_store(file);
  CHECK(s.entries.empty() && !s.error.empty());
  CHECK(update_store(file, [&](Store& st) { st.entries[sha] = {false, "", "", now_utc()}; }, &error));
  CHECK(fs::exists(dir / "plugin-approvals.json.unreadable"));
  s = load_store(file);
  CHECK(s.error.empty() && s.entries.size() == 1 && !s.entries[sha].approved);
  // Keys are SHA-256 digests; anything else in the file is ignored.
  write_file(file, bytes_of("{\"format\":1,\"plugins\":{\"nothex\":{\"approved\":true},\"" + std::string(64, 'A') +
                            "\":{\"approved\":true},\"" + sha + "\":{\"approved\":\"yes\"}}}"));
  s = load_store(file);
  CHECK(s.error.empty() && s.entries.size() == 1 && s.entries.count(std::string(64, 'a')));
}

static bool open_for_write(const fs::path& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

static bool readable_like_the_game(const fs::path& path) {   // mexft_plugins.c read_file
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

static void test_gate(const fs::path& dir, const TestKey& k) {
  const fs::path src = dir / "fighters", store = dir / "plugin-approvals.json";
  fs::create_directories(src);
  const std::vector<uint8_t> wolf = make_plugin(true, 1), shulk = make_plugin(true, 2), third = make_plugin(false, 3);
  write_file(src / "wolf.mufighter", wolf);
  write_file(src / "shulk.mufighter", shulk);
  write_file(src / "notes.txt", bytes_of("not a plugin"));
  const std::string wolf_sha = sha256_hex(wolf.data(), wolf.size()), shulk_sha = sha256_hex(shulk.data(), shulk.size());
  std::vector<std::string> lines;
  static std::vector<std::string>* sink;
  sink = &lines;
  const Log log = [](const char* line) { sink->push_back(line); };
  std::string error;

  {  // Unapproved and unsigned: nothing reaches the game, and nobody is asked without a prompt (a hidden run).
    Gate gate(store, dir / "run1", {k.public_key}, log);
    const fs::path stage = gate.prepare(src, nullptr);
    CHECK(stage == dir / "run1");
    CHECK(gate.staged().empty());
    CHECK(!fs::exists(stage / "wolf.mufighter") && !fs::exists(stage / "shulk.mufighter"));
    bool said_blocked = false;
    for (const auto& l : lines) said_blocked |= l.find("wolf.mufighter (Wolf, sha256 " + wolf_sha.substr(0, 16) + "): blocked, not approved") != std::string::npos;
    CHECK(said_blocked);
    // Approved: copied, the same bytes, and held so nothing can change or remove it, while the game
    // library can still read it.
    CHECK(update_store(store, [&](Store& s) { s.entries[wolf_sha] = {true, "Wolf", "wolf.mufighter", now_utc()}; }, &error));
    gate.prepare(src, nullptr);
    CHECK(gate.staged().size() == 1 && gate.staged().at("wolf.mufighter") == wolf_sha);
    CHECK(read_all(stage / "wolf.mufighter") == wolf);
    CHECK(!open_for_write(stage / "wolf.mufighter"));
    CHECK(!DeleteFileW((stage / "wolf.mufighter").c_str()));
    CHECK(readable_like_the_game(stage / "wolf.mufighter"));
    CHECK(fs::directory_iterator(stage) != fs::directory_iterator());
    gate.release();
    CHECK(!fs::exists(stage));
  }
  {  // One flipped byte is another file: blocked again.
    std::vector<uint8_t> flipped = wolf;
    flipped[flipped.size() / 2] ^= 1;
    write_file(src / "wolf.mufighter", flipped);
    Gate gate(store, dir / "run2", {k.public_key}, log);
    gate.prepare(src, nullptr);
    CHECK(gate.staged().empty());
    CHECK(examine(src / "wolf.mufighter", load_store(store), {k.public_key}).status == Status::Blocked);
    write_file(src / "wolf.mufighter", wolf);
  }
  {  // A valid signature loads without an approval; a bad one falls back to needing approval.
    write_file(src / "shulk.mufighter.sig", bytes_of(hex(sign(k, shulk).data(), 64) + "\n"));
    Gate gate(store, dir / "run3", {k.public_key}, log);
    gate.prepare(src, nullptr);
    CHECK(gate.staged().count("shulk.mufighter") && gate.staged().at("shulk.mufighter") == shulk_sha);
    CHECK(examine(src / "shulk.mufighter", load_store(store), {k.public_key}).status == Status::Signed);
    // The same signature, with no key that verifies it (the production build today): blocked.
    Gate unsigned_gate(store, dir / "run3b", {}, log);
    unsigned_gate.prepare(src, nullptr);
    CHECK(!unsigned_gate.staged().count("shulk.mufighter"));
  }
  {
    write_file(src / "shulk.mufighter.sig", sign(k, wolf));   // a signature of another file
    Gate gate(store, dir / "run4", {k.public_key}, log);
    gate.prepare(src, nullptr);
    CHECK(!gate.staged().count("shulk.mufighter"));
    const Plugin p = examine(src / "shulk.mufighter", load_store(store), {k.public_key});
    CHECK(p.signature == Signature::Invalid && p.status == Status::Blocked);
    CHECK(update_store(store, [&](Store& s) { s.entries[shulk_sha] = {true, "Shulk", "shulk.mufighter", now_utc()}; }, &error));
    gate.prepare(src, nullptr);
    CHECK(gate.staged().count("shulk.mufighter"));
    fs::remove(src / "shulk.mufighter.sig");
  }
  {  // The prompt: asked once per undecided file; a yes is kept, a no is kept as a refusal and not asked again.
    write_file(src / "third.mufighter", third);
    const std::string third_sha = sha256_hex(third.data(), third.size());
    int asked = 0;
    std::string asked_name;
    const Prompt no = [&](const Plugin& p) { ++asked; asked_name = p.name; return false; };
    Gate gate(store, dir / "run5", {k.public_key}, log);
    gate.prepare(src, &no);
    CHECK(asked == 1 && asked_name == "Wolfy");
    CHECK(!gate.staged().count("third.mufighter"));
    CHECK(load_store(store).entries.count(third_sha) && !load_store(store).entries[third_sha].approved);
    Gate again(store, dir / "run6", {k.public_key}, log);
    const Prompt yes = [&](const Plugin&) { ++asked; return true; };
    again.prepare(src, &yes);
    CHECK(asked == 1);   // refused: not asked again
    CHECK(update_store(store, [&](Store& s) { s.entries.erase(third_sha); }, &error));
    Gate third_run(store, dir / "run7", {k.public_key}, log);
    third_run.prepare(src, &yes);
    CHECK(asked == 2 && third_run.staged().count("third.mufighter"));
    CHECK(load_store(store).entries[third_sha].approved);
    fs::remove(src / "third.mufighter");
  }
}

static void test_cli(const fs::path& dir, const TestKey& k) {
  (void)k;
  const fs::path src = dir / "fighters", settings = dir / "port-settings.ini";
  fs::create_directories(src);
  const std::vector<uint8_t> wolf = make_plugin(true, 9);
  write_file(src / "wolf.mufighter", wolf);
  write_file(src / "readme.mufighter", bytes_of("this is text, not a fighter plugin; it is long enough to pass the size test"));
  const std::string sha = sha256_hex(wolf.data(), wolf.size());
  const std::string s = settings.u8string(), f = src.u8string();
  json out;
  CHECK(run_cli({"--settings-path", s, "--fighters-dir", f, "--list-plugins"}, dir, &out) == 0);
  CHECK(out["ok"] == true && out["action"] == "list" && out["folder"] == f && out["plugins"].size() == 2);
  CHECK(out["approvals_file"] == (dir / "plugin-approvals.json").u8string() && out["signing_keys"].is_number() &&
        out["test_key"].is_boolean() && out["approvals"].empty());
  const json& listed = out["plugins"][1];
  CHECK(listed["file"] == "wolf.mufighter" && listed["sha256"] == sha && listed["status"] == "blocked" &&
        listed["approval"] == "none" && listed["signature"] == "none" && listed["name"] == "Wolf" && listed["size"] == wolf.size());
  CHECK(run_cli({"--settings-path", s, "--approve-plugin", (src / "wolf.mufighter").u8string()}, dir, &out) == 0);
  CHECK(out["ok"] == true && out["plugin"]["sha256"] == sha && out["plugin"]["status"] == "approved");
  CHECK(run_cli({"--settings-path", s, "--fighters-dir", f, "--list-plugins"}, dir, &out) == 0);
  CHECK(out["plugins"][1]["status"] == "approved" && out["approvals"].size() == 1 && out["approvals"][0]["sha256"] == sha);
  CHECK(run_cli({"--settings-path", s, "--approve-plugin", (src / "readme.mufighter").u8string()}, dir, &out) == 1);
  CHECK(out["ok"] == false && out["error"].get<std::string>().find("not a fighter plugin") != std::string::npos);
  CHECK(run_cli({"--settings-path", s, "--approve-plugin", (src / "missing.mufighter").u8string()}, dir, &out) == 1);
  CHECK(run_cli({"--settings-path", s, "--revoke-plugin", sha}, dir, &out) == 0);
  CHECK(out["ok"] == true && out["was"] == "approved");
  CHECK(run_cli({"--settings-path", s, "--fighters-dir", f, "--list-plugins"}, dir, &out) == 0);
  CHECK(out["plugins"][1]["status"] == "blocked" && out["plugins"][1]["approval"] == "refused");
  CHECK(run_cli({"--settings-path", s, "--revoke-plugin", "1234"}, dir, &out) == 2 && out["ok"] == false);
  CHECK(run_cli({"--list-plugins", "--revoke-plugin", sha}, dir, &out) == 2);
  CHECK(run_cli({"--settings-path", s, "--fighters-dir", "none", "--list-plugins"}, dir, &out) == 0);
  CHECK(out["folder"].is_null() && out["plugins"].empty());
  CHECK(run_cli({"--settings-path", s, "--vanilla-only", "--list-plugins"}, dir, &out) == 0 && out["folder"].is_null());
  // No --fighters-dir: "fighters" beside the program.
  CHECK(run_cli({"--settings-path", s, "--list-plugins"}, dir, &out) == 0 && out["folder"] == (dir / "fighters").u8string());
  // --print-info's fields (this process has no such options: the folder beside the "program").
  const std::string fields = print_info_fields(dir);
  const json info = json::parse("{\"x\":0" + fields + "}", nullptr, false);
  CHECK(info.is_object() && info["plugins_folder"] == (dir / "fighters").u8string() && info["plugins_blocked"].is_array() &&
        info["plugin_signing_keys"].is_number() && info["plugin_test_key"].is_boolean() && info["plugin_approvals"].is_string());
  fs::remove(src / "readme.mufighter");
}

int main() {
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const fs::path dir = fs::path(temp) / ("mu_plugin_gate_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  test_sha256();
  const TestKey k = test_key();
  test_signatures(k);
  test_name();
  fs::create_directories(dir / "store");
  test_store(dir / "store");
  fs::create_directories(dir / "gate");
  test_gate(dir / "gate", k);
  fs::create_directories(dir / "cli");
  test_cli(dir / "cli", k);
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (g_failures) {
    std::fprintf(stderr, "plugin_gate_test: %d failures\n", g_failures);
    return 1;
  }
  std::printf("plugin_gate_test: ok\n");
  return 0;
}
