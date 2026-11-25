// The source port inside the full application: the same window, renderers (D3D12 with DLSS, D3D11),
// sub-frame presentation, input, settings panel and pacing as the recompiled build, with the game
// itself coming from melee_game.dll (the decompiled sources built natively, see sourceport/game)
// instead of the translated guest. main.cpp calls reserve_memory() first thing and run() where the
// recompiled build would enter __start.
//
// What still differs from the recompiled build is listed where it happens: no memory card yet
// (M10), Slippi's game side not present (M12).
//
// The host is split by topic: source_host_*.cpp, one file per family of commands and services, and
// source_host_internal.h, what they share. This file holds the host's state (every variable, in the
// order the one file had them) and h_slippi_command, the game's command channel: each command is
// one line here, the longer ones a function in their family's file.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- the game library, the machine, rollback re-simulation, --online-test (source_host_boot.cpp,
// source_host_console.cpp, source_host_savestates.cpp, source_host_online.cpp) ----
GameImageReservation g_game_image_reservation;

MuGameApi g_game{};
std::filesystem::path g_loaded_game_path;
#ifdef __APPLE__
mac_elf::Image g_mac_image;
std::jmp_buf g_mac_run_boundary;
bool g_mac_running = false;
std::string g_dll = (mac_host::resources() / "melee_game.so").string();
#else
#ifdef _WIN32
// The compatibility build (melee_source_compat.exe) loads its own SSE2 game library, as on Linux.
#ifndef MELEE_CPU_BASELINE_NAME
#define MELEE_CPU_BASELINE_NAME "AVX2" // standalone syntax/probe builds; CMake supplies the release value
#endif
std::string g_dll = std::strcmp(MELEE_CPU_BASELINE_NAME, "AVX2") == 0 ? "melee_game.dll" : "melee_game_compat.dll";
#elif defined(MELEE_GAME_LIBRARY_NAME)
std::string g_dll = MELEE_GAME_LIBRARY_NAME;   // the compatibility build loads its own game library
#else
std::string g_dll = "melee_game.so";
#endif
#endif
audio_core::State g_audio;
// Native online play (host API 13). While the game re-simulates after a rollback load, the
// machine stands still: no time, alarms, audio, retraces or presentation.
native_savestate::Engine g_savestates;

bool g_resim = false;
bool gl_costs_enabled() {
  // First use follows session-policy setup; global initialization precedes it.
  static const bool on = [] { const char* v = host::test_env::getenv("MELEE_GL_COSTS"); return v && *v == '1'; }();
  return on;
}
uint64_t g_resim_gx_bytes = 0;
uint64_t g_resim_phases = 0, g_resim_polls = 0, g_resim_vi_waits = 0;
// --online-test: a Slippi mode id (the harness enters the match without menus), or -1.
int g_online_test_mode = -1;
uint8_t g_online_test_character = 2, g_online_test_color = 0;
bool g_online_test_started = false;
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
MuSquadSide g_online_test_squad{};
bool g_online_test_squad_cancel = false;
#endif

// ---- the disc's filesystem table, for the game's entry numbers ----
std::vector<FstFile> g_fst;
std::unordered_map<std::string, int32_t> g_paths;   // lower-case "/dir/name" -> entry number
std::unordered_map<uint32_t, std::string> g_music_paths;  // immutable HPS offset -> disc path
std::vector<uint8_t> g_fst_raw;                      // the disc's table as read, for the cosmetic layer
#if MU_SQUAD
std::unordered_map<std::string, int32_t> g_disc_paths;  // the disc's own table: path -> its entry there
#endif

// ---- content views ----
// A mod profile changes some of the disc's files. Every changed file also keeps an entry for its
// retail copy (a separate entry number, so nothing the game cached from the mod is reused), and a
// file only the mod has gets -1. While the retail view is on (online modes that play the retail
// game), opening a path gives the retail entry. Paths Slippi's menu layer serves are the same in
// both views.
std::unordered_map<int32_t, int32_t> g_view_alias;   // mod entry -> retail entry, or -1
bool g_retail_view = false;
// A pack's costume files that only change looks stay on in the retail view (pack_skin_rule.h): the
// same rule a skin from the Mods tab follows, so a skin behaves the same wherever it came from.
// Every costume file of the loaded packs has a verdict here, served or not, for the log and the panel.
std::vector<skins::Verdict> g_pack_skins;
std::unordered_set<int32_t> g_pack_skin_served;
// A pack stage rebuilt around the same stage (Animelee CE's legal stages): online it is served as the
// original stage file with the pack's meshes drawn over it (stage_online_form, stage_graft.h), which
// the gameplay comparison proves plays the same. Pack entry -> the entry serving that grafted copy,
// kept in memory in the top of the system-file range.
std::unordered_map<int32_t, int32_t> g_pack_online_entry;
std::vector<PackOnlineFile> g_pack_online_files;
uint32_t g_pack_online_next = kPackOnlineBase;

// ---- mod layers (source_host_mod_overlay.cpp) ----
slippi::online::NativeGameplayProfile g_mod_gameplay_profile = slippi::online::NativeGameplayProfile::Vanilla;
std::string g_mod_display_name;

// ---- loose-file mods ----
// A file placed at Mods/Files/<disc path> (beside the game, same folder layout as the disc) is
// read instead of the disc's copy: costumes, textures, stages and other data-only mods, no code.
// A file the disc does not have is added. Each gets an offset range past the end of any
// GameCube disc, and reads there are served from memory, so the game's own loader is unchanged.
ModOverlay g_mod_overlay;
std::filesystem::path g_mod_root = std::filesystem::path("Mods") / "Files";
bool g_mod_root_explicit = false;
// Every mod layer in load order: the profile's (--mod-profile), then --mod-iso/--mod-dir/--mod-gci
// in command-line order, then Mods/Files when it exists. A later layer wins over an earlier one.
std::string g_mod_profile_name;
std::vector<mods::Layer> g_mod_layers;
std::vector<mods::Layer> g_explicit_mod_layers;
std::vector<std::filesystem::path> g_mod_gcis;
std::string g_mod_fingerprint;                 // content identity of the enabled layers, "" = vanilla
std::string g_direct_fingerprint;              // what Direct compares when not the content identity (Melee Maker)
bool g_cosmetic_pack = false;                  // a verified official look; standard gameplay online
bool g_training_only = false;                  // only training mods on: online plays the standard game
std::filesystem::path g_profile_card;          // the profile's own card folder, "" = the ordinary card
std::vector<mods::CardImport> g_card_imports;

// ---- cosmetic packs (source_host_cosmetics.cpp) ----
std::vector<CosmeticFile> g_cosmetic_files;
// Entry number -> a second entry for the same file, for online play: the disc's copy for an override
// that is not used online, or the online form of a costume served online in another form
// (host::cosmetics::has_online_form). While the online rule is active, opening the override gives the
// alias: a different entry number, so a copy the game already cached under the override's number is
// not reused online.
std::unordered_map<int32_t, int32_t> g_cosmetic_alias;
// Kept past startup for the character select's L / R skin cycling (republish_cosmetic_slot): the
// table with the lengths published so far, the next free address of the cosmetic range, the path of
// each entry of the disc's table, and the entry that serves each (disc entry, skin) pair, so a skin
// picked again reuses its entry and its range ("" is the disc's own file).
std::vector<uint8_t> g_cosmetic_fst;
uint64_t g_cosmetic_next = kCosmeticBase;
std::vector<std::string> g_raw_paths;
std::map<std::pair<uint32_t, std::string>, int32_t> g_cosmetic_live;

// ---- Slippi's system files and the online menus (source_host_disc.cpp, source_host_online.cpp) ----
bool g_resume_online = false;
bool g_slippi_menus_requested = true;   // --slippi-menus on|off
bool g_slippi_menus = false;            // requested and the layer loaded
std::vector<SystemFile> g_system_files;

// ---- menu mods (source_host_menu_mods.cpp) ----
std::vector<MenuModFile> g_menu_files;
uint32_t g_menu_next = kMenuModBase;
std::unordered_map<std::string, int32_t> g_menu_paths;        // "/mnslchr.usd" -> the entry serving the mod
std::unordered_map<std::string, std::string> g_menu_served;   // the same path -> the mod's id and SHA-256
bool g_menu_mods_off = false;

// ---- new stages (source_host_new_stages.cpp) ----
NewStageSlot g_new_stage_slots[2];
int g_new_stage_last_slot = -1;                 // the slot written last
int32_t g_new_stage_entry = -1;                 // the entry the base's path opens while one is served
std::string g_new_stage_path;                   // that path ("/grtca.dat")
std::string g_new_stage_pick;                   // the new stage picked on the stage select, "" none
std::vector<std::string> g_new_stage_listed;    // the ids of the list the stage select read last, in its order
std::map<std::string, std::vector<uint8_t>> g_new_stage_icons;   // by id, for the stage select's visit

// ---- installed fighters and their own sounds (source_host_roster.cpp) ----
std::vector<RosterFile> g_roster_files;
uint32_t g_roster_next = kRosterBase;
std::vector<RosterMemoryFile> g_roster_memory;
host::fighter_sound::Session g_sound_session;
std::vector<std::vector<uint8_t>> g_sound_tails;      // per installed fighter, after its record
uint32_t g_sound_serial = 0;                          // 0: the disc's own sounds are served
std::map<std::string, int32_t> g_sound_replaced;      // path -> the entry it had (-1: none)
std::map<std::pair<std::string, uint32_t>, int32_t> g_sound_entries;   // (path, contents) -> its entry
bool g_disc_sounds_read = false;
host::fighter_sound::DiscSounds g_disc_sounds;

// ---- Training Mode CE's data files (source_host_practice.cpp) ----
std::vector<TrainingFile> g_training_files;

// ---- stopping (source_host_boot.cpp; guarded is in source_host_internal.h) ----
int g_exit_code = 0;
void (*g_shutdown)(int) = nullptr;
// Replay playback: the options the replay was recorded with ("muOptions"), set by set_replay.
bool g_replaying = false;
uint32_t g_replay_feature_options = 0;
uint32_t g_replay_feature_options2 = 0;
uint32_t g_replay_feature_options3 = 0;
// The CPU AI picker (shim/mu_cpu_ai.c, host command 0xE7): the word the replay was recorded with
// ("muCpuAi", four bits a port), and the word the match being played now runs with, for its recording.
uint32_t g_replay_cpu_ai = 0;
uint32_t g_cpu_ai_match = 0;

// ---- the MEM1 watch diagnostic (source_host_savestates.cpp) ----
Mem1Watch g_mem1_watch;
std::vector<DiscLoad> g_disc_loads;

#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
// Match setup precedes 0x36. Keep this separate from the recording still being saved.
bool g_squad_replay_pending = false;
// Squad Strike in Direct: the agreed setup the next recording keeps ("muSquad"), so it plays back.
std::vector<uint8_t> g_squad_replay_setup_pending;
#endif

// ---- --replay: Slippi replay playback and the recording of every match (source_host_replays.cpp) ----
#ifndef MELEE_NO_SLIPPI
std::string g_replay_path;
std::unique_ptr<Slippi::SlippiGame> g_replay;
MuReplayStart g_replay_start{};
std::vector<uint8_t> g_replay_game_start;   // the original Game Start payload (command 0x36), reused
std::vector<uint8_t> g_replay_game_end;
slippi::NativeReplayStream g_replay_stream;
bool g_replay_written = false;
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
// Applies only to the recording begun by the last Game Start.
bool g_replay_squad = false;
// Replay playback: the Squad Strike in Direct setup the replay recorded ("muSquad"), empty for any other.
std::vector<uint8_t> g_replay_squad_setup;
#endif
// The current recording goes to the spectator server only (an offline match with MELEE_SOURCE_RECORD
// "2"): write_replay_recording leaves it unwritten.
bool g_replay_spectator_only = false;
#endif
#ifndef MELEE_NO_SLIPPI
std::map<int32_t, slippi::NativeReplayStream::Mark> g_replay_marks;   // per kept state: the recording then
#endif

}  // namespace host_internal

#ifndef MELEE_NO_SLIPPI
std::string g_replay_mod_profile;
#endif

namespace host_internal {

// ---- big-endian helpers the files share ----
uint32_t read_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void append_be32(std::vector<uint8_t>& q, uint32_t v) { q.push_back((uint8_t)(v >> 24)); q.push_back((uint8_t)(v >> 16)); q.push_back((uint8_t)(v >> 8)); q.push_back((uint8_t)v); }

// ---- the game -> host command channel (MU_CMD_*, runtime/abi/mu_commands.h) ----
// The ids themselves, with who sends each: runtime/abi/mu_commands.h.
// The replay viewer's controls (shim/mu_replay.c mu_replay_frame_begin, host/replay_control.h):
// MU_CMD_REPLAY_GATE: payload s32 next replay frame (BE), u8 0 ask / 1 restore now. Defined with the
// replay host (source_host_replays.cpp).

int32_t h_slippi_command(uint8_t command, const uint8_t* payload, uint32_t payload_size,
                         uint8_t* response, uint32_t response_capacity, uint32_t* response_size) {
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  // Handle locally even in MU_NO_SLIPPI; no online command validation or network I/O.
  if (command == MU_SQUAD_COMMAND && payload && payload_size == 2 &&
      payload[0] == MU_SQUAD_OP_REPLAY_MATCH && payload[1] <= 2) {
    // 1: a squad match a replay cannot play back (not kept); 2: Squad Strike in Direct, kept with the
    // setup it was played with (the agreed one, or the one a replay being played recorded).
    g_squad_replay_pending = payload[1] == 1;
    g_squad_replay_setup_pending.clear();
#ifndef MELEE_NO_SLIPPI
    if (payload[1] == 2) g_squad_replay_setup_pending = g_replaying ? g_replay_squad_setup : slippi::online::squad_match_setup();
#endif
    if (payload[1] == 2 && g_squad_replay_setup_pending.empty()) g_squad_replay_pending = true;   // nothing to replay it with
    if (response_size) *response_size = 0;
    return 0;
  }
  // The squadLineup agent-test key, for the online squad CSS. Builds without the hook answer
  // nothing, so the CSS stays a player's.
  if (command == MU_SQUAD_COMMAND && payload && payload_size == 1 &&
      payload[0] == MU_SQUAD_OP_AGENT_LINEUP) {
    if (response_size) *response_size = 0;
    return 0;
  }
  // Squad Strike in Direct: the agreed setup of the match being set up (mu_squad_wire.h version 4),
  // nothing for a normal match. Host-local, never sent to a peer.
  if (command == MU_SQUAD_COMMAND && payload && payload_size == 1 &&
      payload[0] == MU_SQUAD_OP_MATCH_SETUP) {
    std::vector<uint8_t> setup;
#ifndef MELEE_NO_SLIPPI
    // A replay being played: the setup it recorded ("muSquad"); else this online match's.
    setup = g_replaying ? g_replay_squad_setup : slippi::online::squad_match_setup();
#endif
    if (!response || setup.size() > response_capacity) setup.clear();
    if (!setup.empty()) std::memcpy(response, setup.data(), setup.size());
    if (response_size) *response_size = (uint32_t)setup.size();
    return 0;
  }
  // ... and this player's lineup on the online select (MU_SQUAD_OP_DIRECT_LINEUP), for the peer.
  if (command == MU_SQUAD_COMMAND && payload && payload_size >= 2 && payload[0] == MU_SQUAD_OP_DIRECT_LINEUP &&
      payload[1] <= MU_SQUAD_LINEUP && payload_size == 2u + 2u * payload[1]) {
    MuSquadSide side{};
    side.count = payload[1];
    for (unsigned i = 0; i < side.count; ++i) side.members[i] = {payload[2 + 2 * i], payload[3 + 2 * i]};
#ifndef MELEE_NO_SLIPPI
    // A lineup that is not one (a fighter twice, a color it lacks) counts as none: no older one stays.
    if (side.count == 0 || mu_squad_lineup_valid(&side)) slippi::online::set_squad_lineup(payload + 2, payload[1]);
    else slippi::online::set_squad_lineup(nullptr, 0);
#endif
    if (response_size) *response_size = 0;
    return 0;
  }
#endif
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  // Read-only harness samples are consumed here, never validated/sent as Slippi commands.
  if (command == MU_SQUAD_COMMAND && payload && payload_size == 9 &&
      (payload[0] == MU_SQUAD_OP_DIGEST_FRAME || payload[0] == MU_SQUAD_OP_DIGEST_END)) {
    bool direct_squad = false;   // Squad Strike in Direct (MELEE_TEST_ONLINE_SQUAD on local peering)
#ifndef MELEE_NO_SLIPPI
    direct_squad = !slippi::online::squad_match_setup().empty();
#endif
    if ((g_online_test_squad.count || direct_squad) && slippi::Matchmaking::local_peer.enabled)
      host::squad_digest_observe((int32_t)read_be32(payload + 1), (int32_t)read_be32(payload + 5),
                                 payload[0] == MU_SQUAD_OP_DIGEST_END);
    if (response_size) *response_size = 0;
    return 0;
  }
#endif
  return slippi::online::native_command(
      command, payload, payload_size, response, response_capacity, response_size,
      MU_SLIPPI_RESPONSE_CAPACITY, true,
      [](uint8_t c, const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
        // Music: the game hands its songs to the jukebox as the static recomp's Slippi codes do
        // (StartSong D6, Stop D7, VolumeChange D8; shim/mu_audio.c).
        if (c == MU_CMD_MUSIC_PLAY && n == 8) return music_play_command(p);
        if (c == MU_CMD_MUSIC_STOP) { slippi::jukebox::stop(); return true; }
        if (c == MU_CMD_MUSIC_VOLUME && n == 1) { slippi::jukebox::set_melee_volume(p[0]); return true; }
        if (c == slippi::online::CMD_CAPTURE_SAVESTATE) { native_capture((int32_t)read_be32(p)); return true; }
        if (c == slippi::online::CMD_LOAD_SAVESTATE) { native_load((int32_t)read_be32(p), p); return true; }
        if (c == MU_CMD_SELFTEST_KEEP && n == 4) { g_savestates.keep_reference((int32_t)read_be32(p)); return true; }
        if (c == MU_CMD_LAB_SAVE) { lab_save(); return true; }
        if (c == MU_CMD_LAB_LOAD) { lab_load(); return true; }
        if (c == MU_CMD_REPLAY_GATE && n == 5) { reply.push_back(replay_gate((int32_t)read_be32(p), p[4])); return true; }
        if (c == MU_CMD_REPLAY_DEFENSE) return replay_defense_command(reply);
        if (c == MU_CMD_FRAME_EVENT) return frame_event_command(p, n);
        if (c == MU_CMD_STAGE_SONGS && n == 16) {
          slippi::jukebox::note_stage_songs((int32_t)read_be32(p), (int32_t)read_be32(p + 4));
          slippi::jukebox::note_stage_songs((int32_t)read_be32(p + 8), (int32_t)read_be32(p + 12));
          return true;
        }
        if (c == MU_CMD_LAB_ADVANTAGE && n == 5) { training_overlay::set_advantage((int32_t)read_be32(p), p[4]); return true; }
        if (c == MU_CMD_ONLINE_OPTION && n == 2) return online_option_command(p, reply);
        if (c == MU_CMD_ONLINE_CONTENT) return online_content_command(p, n, reply);
        if (c == MU_CMD_CSS_FRAME) slippi::online::gecko_pump();   // Gecko codes for the match (gecko_session.h)
        if (c == MU_CMD_CSS_FRAME && n == 0) { walter_egg::set_camera(nullptr); reply.push_back(walter_egg::css_frame()); return true; }
        if (c == MU_CMD_CSS_FRAME && n == 13 * sizeof(float)) {   // with the menu camera (shim mu_menu_camera): the C-stick's turn
          float camera[13];
          std::memcpy(camera, p, sizeof camera);
          walter_egg::set_camera(camera);
          reply.push_back(walter_egg::css_frame());
          return true;
        }
        if (c == MU_CMD_CSS_FRAME && n == 5) {   // with five bytes: a hand moved (the port, then x and y in tenths)
          walter_egg::set_hand(p[0], (int16_t)(p[1] << 8 | p[2]), (int16_t)(p[3] << 8 | p[4]));
          return true;
        }
        if (c == MU_CMD_ZELDA_AS_SHEIK && n == 0) return zelda_as_sheik_command(reply);
        if (c == MU_CMD_BUTTON_LABEL && n == 3) {   // shim/mu_slippi_css.c: the key bound to a button, keyboard ports only
          char name[16];
          if (host::input_keyboard_button_label(p[0], (uint16_t)(p[1] << 8 | p[2]), name, sizeof name))
            for (const char* q = name; *q; ++q) reply.push_back((uint8_t)*q);
          return true;
        }
        if (c == MU_CMD_PRACTICE_CPU) return practice_cpu_command(p, n, reply);   // shim/mu_practice_cpu.c: the settings, or what it is doing
        if (c == MU_CMD_CPU_AI && n >= 1) return cpu_ai_command(p, n, reply);
        if (c == MU_CMD_MOD_BROWSER && n >= 1) { mod_browser_command(p, n, reply); return true; }
        if (c == MU_CMD_LOBBY && n >= 1) { lobby_command(p, n, reply); return true; }
        if (c == MU_CMD_NEW_STAGES && n >= 1) { new_stages_command(p, n, reply); return true; }
        if (c == MU_CMD_SKIN_CYCLE && (n == 4 || (n == 5 && p[4] != 0))) {
          cycle_costume_skin(p[1], p[2], p[3] ? 1 : -1, reply, n == 5 ? p[4] : 0);
          return true;
        }
        if (c == MU_CMD_STAGE_SKIN_MATCH && n >= 2 && n <= 64 && p[n - 1] == 0) {
          plan_random_stage_skin(std::string((const char*)p, strnlen((const char*)p, n)), reply);
          return true;
        }
        if (c == MU_CMD_STAGE_SKIN_CYCLE && n >= 2 && n <= 65) {
          cycle_stage_skin(std::string((const char*)p + 1, strnlen((const char*)p + 1, n - 1)), p[0] ? 1 : -1, reply);
          return true;
        }
        if (c == slippi::online::CMD_NATIVE_MENU_FLAGS && n == 0) {
          // An empty request reads the local test policy. Other 0xFA commands have a payload.
          return slippi::online::handle(c, p, n, reply);
        }
        if (c == MU_MAKER_COMMAND && n == 1 && p[0] == MU_MAKER_OP_CACHE) return maker_cache_command(reply);
        if (c == MU_MAKER_COMMAND) {
          maker::command(p, n, reply);   // Melee Maker's data for its native engine (maker_source.h)
          return true;
        }
        if (c == MU_CMD_TE_MENU_MUSIC && n == 1) return te_menu_music_command(p, reply);
        if (c == MU_CMD_TE_SETTINGS && (n == 0 || n == 8)) return te_settings_command(p, n, reply);
        if (c == MU_CMD_CONTENT_MODE && n == 1) {
          if (p[0] != 0xFE) apply_content_mode(p[0] == 0xFF ? -1 : (int)p[0]);   // 0xFE: query only
          reply.push_back(g_retail_view ? 1 : 0);
          return true;
        }
        if (c == MU_CMD_SELFTEST_COMPARE && n == 4) { report_selftest((int32_t)read_be32(p), reply); return true; }
        if (c == slippi::online::CMD_ONLINE_INPUTS) online_inputs_command(p);
#ifndef MELEE_NO_SLIPPI
        // A peer-to-peer session (--p2p-*, source_p2p.cpp) installed its handler: it owns the
        // session commands for this run, and Slippi's session code is never entered. The handler is
        // only ever installed after such a session started, so every other run takes the line below.
        if (host::netplay::has_command_handler()) return host::netplay::handle(c, p, n, reply);
#endif
        return slippi::online::handle(c, p, n, reply);
      });
}

}  // namespace host_internal
}  // namespace source_port
