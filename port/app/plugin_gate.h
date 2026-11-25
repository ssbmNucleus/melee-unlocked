// Fighter plugin approval. A .mufighter plugin is a fighter's native code that the game library links
// into itself and runs (sourceport/game/mexft/mexft_plugins.c), so it runs with the player's rights on
// this PC. The owner's rule ("option C"): a plugin signed by SSBM Nucleus loads silently; an unsigned
// one loads only after the player approved that exact file.
//
// How the host enforces it without touching the game library: the library asks the host which folder
// to load plugins from (CMD_MOD_BROWSER 6, source_host_mods_menu.cpp). The host answers with a folder of its own
// for this run, into which it copies only the plugins that pass, under their own names (so the online
// plugin digest, name plus contents, is unchanged). Each plugin is read once into memory, hashed and
// checked there; the copy is written from those bytes, then opened again with writes and deletes
// shut out (FILE_SHARE_READ only), compared byte for byte with what was checked, and kept open like
// that for the rest of the run. So the file the game library reads is the file that was approved.
//
// Files (docs/fighter-plugins.md):
//   plugin-approvals.json beside the settings file (--settings-path, default port-settings.ini in the
//     working folder): {"format": 1, "plugins": {"<sha256 of the plugin file>": {"approved": true,
//     "name": "Wolf", "file": "wolf.mufighter", "date": "2026-11-23T04:00:00Z"}}}. approved false is a
//     refusal (the player said no, or --revoke-plugin): blocked and not asked again.
//   <plugin>.sig beside the plugin (wolf.mufighter.sig): a detached Ed25519 signature (RFC 8032), 64
//     bytes raw or 128 hex digits, over the 40 bytes "MUFTSIG1" + SHA-256(plugin file). Checked against
//     the public keys compiled in (plugin_keys.cpp; the production slot is empty until the owner fills it).
//   plugins-run\<process id>\ beside the settings file: this run's gated copies.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace plugin_gate {

using Key = std::array<uint8_t, 32>;

// ---- hashing ----
void sha256(const uint8_t* data, size_t size, uint8_t out[32]);
std::string sha256_hex(const uint8_t* data, size_t size);
std::string hex(const uint8_t* data, size_t size);
bool is_sha256_hex(const std::string& text);   // 64 hex digits

// ---- signatures ----
enum class Signature { None, Valid, Invalid };
const char* signature_name(Signature s);       // "none", "valid", "invalid"
// What a signature covers: "MUFTSIG1" and the 32-byte SHA-256 of the plugin file.
std::vector<uint8_t> signed_message(const uint8_t digest[32]);
// A .sig file's contents (64 raw bytes, or 128 hex digits with whitespace around them) checked against
// `keys`. Empty contents: None. Anything that does not verify against any key: Invalid.
Signature check_signature(const uint8_t digest[32], const std::vector<uint8_t>& sig_file, const std::vector<Key>& keys);
// The keys this build trusts (plugin_keys.cpp): none until the owner supplies Nucleus's key.
const std::vector<Key>& builtin_keys();
bool builtin_test_key();   // built with -DMELEE_PLUGIN_TEST_KEY=ON (test runs only)

// ---- the approval store ----
struct Approval {
  bool approved = false;
  std::string name, file, date;
};
struct Store {
  std::map<std::string, Approval> entries;   // by lowercase SHA-256
  std::string error;                          // the file exists but cannot be read: nothing is approved
};
std::filesystem::path store_path(const std::string& settings_path);
Store load_store(const std::filesystem::path& path);
// Read, change and write the store under a lock file, replacing it whole (never half written).
bool update_store(const std::filesystem::path& path, const std::function<void(Store&)>& change, std::string* error);
std::string now_utc();   // 2026-11-23T04:00:00Z

// ---- one plugin file ----
enum class Status { Signed, Approved, Blocked };
const char* status_name(Status s);             // "signed", "approved", "blocked"
struct Plugin {
  std::string file;                 // its name in the folder
  std::filesystem::path path;
  std::string name;                 // the fighter (the plugin's record; its file name when unreadable)
  std::string sha256;
  uint64_t size = 0;
  Signature signature = Signature::None;
  int approval = 0;                 // 1 approved, -1 refused, 0 never decided
  std::string approval_date;
  Status status = Status::Blocked;
  std::string problem;              // set when the file could not be read (then nothing else is)
  std::vector<uint8_t> bytes;       // exactly what was hashed
};
// The whole file in one read, with writes shut out while it is read.
bool read_whole(const std::filesystem::path& path, std::vector<uint8_t>* out, std::string* error);
// A 64-bit COFF object that names the plugin record (what the mod browser checks before keeping one).
bool looks_like_plugin(const std::vector<uint8_t>& bytes);
// The fighter's name from the plugin's record (its m-ex name, else its descriptor's): "" when the
// object does not say. Bounds-checked: the bytes are not trusted.
std::string fighter_name(const std::vector<uint8_t>& bytes);
Plugin examine(const std::filesystem::path& file, const Store& store, const std::vector<Key>& keys);
// Every *.mufighter in `folder` (not its subfolders), by name.
std::vector<Plugin> scan(const std::filesystem::path& folder, const Store& store, const std::vector<Key>& keys);

// ---- where plugins come from ----
// Without --fighters-dir: MELEE_FIGHTER_PLUGINS when set, else "fighters" beside the game library
// (the library's own rule before the host gated it).
std::filesystem::path default_plugin_folder(const std::filesystem::path& library_dir);

// ---- this run ----
using Prompt = std::function<bool(const Plugin&)>;   // true: the player allowed it
using Log = void (*)(const char* line);
// The settings file this run uses (main.cpp, once the command line is read).
void configure_run(const std::string& settings_path, Log log);
// This run's plugin folder for the game library: the signed and approved plugins of `source`, held
// unchanged. Empty: no plugins this run (nothing could be prepared). `ask` set: a plugin nobody decided
// on yet is put to the player first (an interactive start only).
std::filesystem::path run_folder(const std::filesystem::path& source, const Prompt* ask);
// The game is over: closes this run's copies and removes their folder (the process ends without
// running destructors). A folder a crash leaves is removed at the next start.
void end_run();
// Whether a plugin file would load (signed, or approved): the mod browser's note after an install.
bool would_load(const std::string& path);
// The plain yes/no question (Windows message box).
bool ask_player(const Plugin& plugin);

// A gate on its own, for the run and for the tests: one copy folder, the store, the keys.
class Gate {
 public:
  Gate(std::filesystem::path store, std::filesystem::path stage, std::vector<Key> keys, Log log = nullptr);
  ~Gate();
  Gate(const Gate&) = delete;
  Gate& operator=(const Gate&) = delete;
  // Copies what passes from `source` into the stage folder; returns the stage folder ("" on failure).
  std::filesystem::path prepare(const std::filesystem::path& source, const Prompt* ask);
  const std::filesystem::path& stage() const { return stage_; }
  // The files staged so far: name -> SHA-256.
  std::map<std::string, std::string> staged() const;
  // Closes the held copies and removes the stage folder.
  void release();

 private:
  bool place(const Plugin& plugin);
  void say(const std::string& key, const std::string& line);
  std::filesystem::path store_, stage_;
  std::vector<Key> keys_;
  Log log_;
  struct Held { std::string sha256; void* handle; };
  std::map<std::string, Held> held_;
  std::map<std::string, std::string> said_;
  std::map<std::string, bool> declined_;
};

// ---- command line (main.cpp) ----
// --approve-plugin <path>, --list-plugins, --revoke-plugin <sha256>, with --settings-path and
// --fighters-dir as a run would take them. Each prints one JSON object and exits (0 done, 1 failed,
// 2 bad command line).
bool cli_requested(int argc, char** argv);
int cli_run(int argc, char** argv, const std::filesystem::path& program_dir, std::string* json);
// The plugin fields of --print-info (",\"plugins_blocked\":[...]..."), from the command line of this
// process. Never throws.
std::string print_info_fields(const std::filesystem::path& program_dir);

}  // namespace plugin_gate
