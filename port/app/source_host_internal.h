// The Source Port host's files (source_host*.cpp) share this: the includes, the host's types and
// constants, its state (extern, defined in source_host.cpp) and the functions one file defines and
// another calls, under the file that defines them. source_host.h is the host's interface; nothing
// outside source_host*.cpp includes this.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "test_env.h"
#include "layer_fingerprint.h"
#include "source_host.h"
#include <windows.h>
#ifdef _WIN32
#include "plugin_gate.h"   // fighter plugins: only signed or player-approved ones reach the game library
#endif
#ifndef _WIN32
#include <dlfcn.h>
#ifdef __APPLE__
#include "../mac/elf_loader.h"
#include "../mac/platform_paths.h"
#include "../mac/context_registers.h"
#include <csetjmp>
#else
#include <link.h>
#endif
#include <csignal>
#endif
#include <algorithm>
#include <atomic>
#include <deque>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "host.h"
#include "product_identity.h"
#include "sim_work.h"
#include "rollback_perf.h"
#include "audio.h"
#include "console_sound_mode.h"
#include "audio_core.h"
#include "ax_ucode.h"
#include "gx_core.h"
#include "gx_retro.h"
#include "gx_shader.h"
#include "render_options.h"
#include "training_overlay.h"
#include "practice_cpu.h"
#include "lcancel.h"
#include "offline_input_delay.h"
#include "disc_archive.h"
#include "user_gecko.h"
#include "mu_host.h"
#include "mu_commands.h"   // every command id of the game -> host channel
#include "mu_host_aram.h"
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#include "mu_squad_wire.h"
#endif
#include "../runtime/abi/mu_tourney.h"
#include "ppc.h"
#include "render_observer.h"
#include "native_state_layout.h"
#include "native_savestate.h"
#include "walter_egg.h"
#include "gecko_session.h"
// Both builds: the neutral netplay state. In the build without the Slippi layer it also answers the
// Slippi names; in the normal build a peer-to-peer session (--p2p-*) is found through it.
#include "netplay_state.h"
#ifndef MELEE_NO_SLIPPI
#include "native_practice.h"
#include "jukebox.h"
#include "slippi_playback.h"
#include "slippi_online.h"
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#include "squad_test_spec.h"
#endif
#include "source_p2p.h"
#include "exi_slippi.h"
#include "native_slippi_bridge.h"
#include "native_replay_stream.h"
#include "slippi_spectate.h"
#include "slippi_net.h"
#include "native_recording_codes.h"
#include "slippilib/SlippiGame.h"
#endif
#include "card_backup.h"    // both builds: save backups
#include "source_mod_overlay.h"
#include "mod_profile.h"
#include "net_trace.h"
#include "replay_control.h"
#include "replay_coach.h"
#include "replay_scan.h"
#include "mod_scan.h"
#include "hackpack_source.h"
#include "maker_source.h"
#include "pack_skin_rule.h"
#include "disc_skin_scan.h"
#include "cosmetic_mods.h"
#include "mod_browser.h"
#include "mods_menu.h"        // MU_CMD_MOD_BROWSER 9 to 14: the Mods menu in Trophies' place
#include "release_scope.h"    // the Mods categories this release has (MU_CMD_MOD_BROWSER 8, the runtime gates)
#include "menu_mods.h"        // menu screens installed from the Mods menu's Menus (publish_menu_mods)
#include "new_stages.h"       // new stages from the Mods menu's New Stages (MU_CMD_NEW_STAGES, plan_new_stage)
#include "lobby_directory.h"  // MU_CMD_LOBBY, and MU_CMD_MOD_BROWSER 9 to 14 while the lobby list is open
#include "product_identity.h"
#include "opponent_skins.h"   // MU_CMD_MOD_BROWSER 7: one port's own costume
#include "costume_slots.h"    // MU_CMD_MOD_BROWSER 15 to 18: installed skins as costume slots
#include "roster.h"
#include "online_fighters.h"   // custom fighters online: MU_CMD_ONLINE_CONTENT
#include "fighter_sound.h"   // installed fighters' own sound banks and name calls
#include "stage_dat_safety.h"
#include "pack_audio_safety.h"
#include "texture_pack.h"
#include "window.h"
#include "console_input.h"

// The settings panel's port-code options (pc_settings.cpp); the native game reads them through
// MuHostApi.game_options.
namespace gecko { extern int option_screen_shake_percent; extern bool option_no_screen_shake; extern bool option_pal_stock_icons; extern bool option_widescreen; }

namespace source_port {
namespace host_internal {
constexpr uintptr_t MEM1_BASE = 0x80000000u;
constexpr uint32_t MEM1_SIZE = 40u << 20;   // the console's 24 MB, grown for 8-byte pointers; the game image follows
constexpr uintptr_t LOCKED_CACHE_BASE = 0xE0000000u;
constexpr uint32_t LOCKED_CACHE_SIZE = 16u << 10;
// Right after MEM1, so the game's own statics (the font atlas, static textures) have a physical
// address the GX texture and display-list registers can hold: 26 bits, the first 64 MB.
constexpr uintptr_t GAME_IMAGE_BASE = 0x82800000u;
// The end of what GX can address: the game image must fit below it. The range itself is held from
// startup by g_game_image_reservation (source_host.cpp); with bottom-up address randomization, the
// process's early allocations (the GPU driver's among them) otherwise land anywhere in the low 4 GB.
constexpr uintptr_t GAME_IMAGE_HOLD_END = 0x84000000u;

// The game library must load at GAME_IMAGE_BASE. Everything the host does before loading it (mod
// scans, cosmetics, the music pack index) allocates, and a heap block placed there moved the library
// elsewhere and stopped the Source Port at startup. The range is held from before main and given
// back the moment the library is loaded.
struct GameImageReservation {
  void* base = nullptr;
  GameImageReservation() {
    // Generous: the image's own size is checked again after the load.
#ifndef __APPLE__
    base = VirtualAlloc((void*)GAME_IMAGE_BASE, 0x04000000u, MEM_RESERVE, PAGE_NOACCESS);
#endif
  }
  void release() {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
    base = nullptr;
  }
};

// ---- the host's types: what each kind of served file keeps, and the ranges past the disc they are served from ----
struct FstFile { uint32_t offset, length; bool dir; };
struct PackOnlineFile { uint32_t start; std::vector<uint8_t> data; };
constexpr uint32_t kPackOnlineBase = 0x6C000000u;   // up to the overlay at 0x70000000; system files stay below
// online_form: this range serves the file's online form (a reshaped costume with the standard rest pose).
struct CosmeticFile { uint32_t start, vanilla_start, length; bool logged; bool online_form = false; };
constexpr uint32_t kCosmeticBase = 0xF0000000u;
struct SystemFile {
  std::string path; uint32_t start; std::vector<uint8_t> data;
};

// Between the disc (under 1.5 GB) and the mod overlay range (ModOverlay::base up to the cosmetics at
// 0xF0000000). It used to sit at 0xC0000000, inside the overlay's range: a pack large enough to
// reach it (the 20XX Hack Pack's 776 files) had its files there refused, and the game waited on them.
// It moved from 0x90000000 to 0x60000000 so the overlay could start lower and hold 2 GiB: a large
// m-ex build (Chaos Eternal) changes more than the 1.25 GiB the old range held.
constexpr uint32_t kSystemFileBase = 0x60000000u;
// The files of fighters installed beside the game (host/roster.h): from here up to the pack's
// online copies at kPackOnlineBase. The system files stay below.
constexpr uint32_t kRosterBase = 0x68000000u;
// Training Mode CE's data files (TM/ beside the game, publish_training_files): the last 16 MB below
// the installed fighters. The system files stay below them.
constexpr uint32_t kTrainingFileBase = 0x67000000u;
// Menu mods from the Mods menu (publish_menu_mods): the 32 MB below the new stages. The system files
// stay below them.
constexpr uint32_t kMenuModBase = 0x64000000u;
// New stages from the Mods menu (plan_new_stage): the 16 MB below Training Mode's files, two slots of
// 8 MB used in turn.
constexpr uint32_t kNewStageBase = 0x66000000u;
constexpr uint32_t kNewStageSlot = 0x00800000u;
static_assert(kSystemFileBase >= 0x58000000u && kSystemFileBase + 0x10000000u <= ModOverlay::base,
              "Slippi system files sit past the largest disc and below the mod overlay");
struct MenuModFile { uint32_t start; std::vector<uint8_t> data; std::string name; bool read = false; };
struct NewStageSlot { uint32_t start = 0; std::vector<uint8_t> data; std::string id, name; bool read = false; };
struct RosterFile { uint32_t start, length; std::filesystem::path path; };
struct RosterMemoryFile { uint32_t start; std::vector<uint8_t> data; };
struct TrainingFile { uint32_t start; std::vector<uint8_t> data; };
struct Mem1Watch {
  bool on = false;
  std::vector<std::pair<uintptr_t, uintptr_t>> ranges;   // [start, end) outside the snapshot
  std::vector<uint64_t> hashes;
  std::vector<std::vector<uint8_t>> bytes;               // each range as last seen
};

// Diagnostic for the MEM1 watch: where each disc read landed, so a changed byte outside the
// snapshot can be named by the file loaded there.
struct DiscLoad { uintptr_t dst; uint32_t size, offset; };

// ---- the host's state, defined in source_host.cpp (in the order the one file had it) ----
extern GameImageReservation g_game_image_reservation;
extern MuGameApi g_game;
extern std::filesystem::path g_loaded_game_path;
#ifdef __APPLE__
extern mac_elf::Image g_mac_image;
extern std::jmp_buf g_mac_run_boundary;
extern bool g_mac_running;
#endif
extern std::string g_dll;
extern audio_core::State g_audio;
extern native_savestate::Engine g_savestates;
extern bool g_resim;
bool gl_costs_enabled();
extern uint64_t g_resim_gx_bytes;
extern uint64_t g_resim_phases, g_resim_polls, g_resim_vi_waits;
extern int g_online_test_mode;
extern uint8_t g_online_test_character, g_online_test_color;
extern bool g_online_test_started;
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
extern MuSquadSide g_online_test_squad;
extern bool g_online_test_squad_cancel;
#endif
extern std::vector<FstFile> g_fst;
extern std::unordered_map<std::string, int32_t> g_paths;   // lower-case "/dir/name" -> entry number
extern std::unordered_map<uint32_t, std::string> g_music_paths;  // immutable HPS offset -> disc path
extern std::vector<uint8_t> g_fst_raw;                      // the disc's table as read, for the cosmetic layer
#if MU_SQUAD
extern std::unordered_map<std::string, int32_t> g_disc_paths;  // the disc's own table: path -> its entry there
#endif
extern std::unordered_map<int32_t, int32_t> g_view_alias;   // mod entry -> retail entry, or -1
extern bool g_retail_view;
extern std::vector<skins::Verdict> g_pack_skins;
extern std::unordered_set<int32_t> g_pack_skin_served;
extern std::unordered_map<int32_t, int32_t> g_pack_online_entry;
extern std::vector<PackOnlineFile> g_pack_online_files;
extern uint32_t g_pack_online_next;
extern slippi::online::NativeGameplayProfile g_mod_gameplay_profile;
extern std::string g_mod_display_name;
extern ModOverlay g_mod_overlay;
extern std::filesystem::path g_mod_root;
extern bool g_mod_root_explicit;
extern std::string g_mod_profile_name;
extern std::vector<mods::Layer> g_mod_layers;
extern std::vector<mods::Layer> g_explicit_mod_layers;
extern std::vector<std::filesystem::path> g_mod_gcis;
extern std::string g_mod_fingerprint;                 // content identity of the enabled layers, "" = vanilla
extern std::string g_direct_fingerprint;              // what Direct compares when not the content identity (Melee Maker)
extern bool g_cosmetic_pack;                          // a verified official look; standard gameplay online
extern bool g_training_only;                          // only training mods on: online plays the standard game
extern std::filesystem::path g_profile_card;          // the profile's own card folder, "" = the ordinary card
extern std::vector<mods::CardImport> g_card_imports;
extern std::vector<CosmeticFile> g_cosmetic_files;
extern std::unordered_map<int32_t, int32_t> g_cosmetic_alias;
extern std::vector<uint8_t> g_cosmetic_fst;
extern uint64_t g_cosmetic_next;
extern std::vector<std::string> g_raw_paths;
extern std::map<std::pair<uint32_t, std::string>, int32_t> g_cosmetic_live;
extern bool g_resume_online;
extern bool g_slippi_menus_requested;   // --slippi-menus on|off
extern bool g_slippi_menus;             // requested and the layer loaded
extern std::vector<SystemFile> g_system_files;
extern std::vector<MenuModFile> g_menu_files;
extern uint32_t g_menu_next;
extern std::unordered_map<std::string, int32_t> g_menu_paths;        // "/mnslchr.usd" -> the entry serving the mod
extern std::unordered_map<std::string, std::string> g_menu_served;   // the same path -> the mod's id and SHA-256
extern bool g_menu_mods_off;
extern NewStageSlot g_new_stage_slots[2];
extern int g_new_stage_last_slot;                 // the slot written last
extern int32_t g_new_stage_entry;                 // the entry the base's path opens while one is served
extern std::string g_new_stage_path;              // that path ("/grtca.dat")
extern std::string g_new_stage_pick;              // the new stage picked on the stage select, "" none
extern std::vector<std::string> g_new_stage_listed;    // the ids of the list the stage select read last, in its order
extern std::map<std::string, std::vector<uint8_t>> g_new_stage_icons;   // by id, for the stage select's visit
extern std::vector<RosterFile> g_roster_files;
extern uint32_t g_roster_next;
extern std::vector<RosterMemoryFile> g_roster_memory;
extern host::fighter_sound::Session g_sound_session;
extern std::vector<std::vector<uint8_t>> g_sound_tails;      // per installed fighter, after its record
extern uint32_t g_sound_serial;                              // 0: the disc's own sounds are served
extern std::map<std::string, int32_t> g_sound_replaced;      // path -> the entry it had (-1: none)
extern std::map<std::pair<std::string, uint32_t>, int32_t> g_sound_entries;   // (path, contents) -> its entry
extern bool g_disc_sounds_read;
extern host::fighter_sound::DiscSounds g_disc_sounds;
extern std::vector<TrainingFile> g_training_files;
extern int g_exit_code;
extern void (*g_shutdown)(int);
extern bool g_replaying;
extern uint32_t g_replay_feature_options;
extern uint32_t g_replay_feature_options2;
extern uint32_t g_replay_feature_options3;
extern uint32_t g_replay_cpu_ai;
extern uint32_t g_cpu_ai_match;
extern Mem1Watch g_mem1_watch;
extern std::vector<DiscLoad> g_disc_loads;
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
extern bool g_squad_replay_pending;
extern std::vector<uint8_t> g_squad_replay_setup_pending;   // Squad Strike in Direct: the next recording's "muSquad"
#endif
#ifdef MELEE_NO_SLIPPI
constexpr bool g_replay = false;
#endif
#ifndef MELEE_NO_SLIPPI
extern std::string g_replay_path;
extern std::unique_ptr<Slippi::SlippiGame> g_replay;
extern MuReplayStart g_replay_start;
extern std::vector<uint8_t> g_replay_game_start;   // the original Game Start payload (command 0x36), reused
extern std::vector<uint8_t> g_replay_game_end;
extern slippi::NativeReplayStream g_replay_stream;
extern bool g_replay_written;
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
extern bool g_replay_squad;
extern std::vector<uint8_t> g_replay_squad_setup;   // playback: the replay's "muSquad", else empty
#endif
extern bool g_replay_spectator_only;
extern std::map<int32_t, slippi::NativeReplayStream::Mark> g_replay_marks;   // per kept state: the recording then
#endif

// ---- each file's functions another file calls ----
// source_host.cpp
uint32_t read_be32(const uint8_t* p);
void append_be32(std::vector<uint8_t>& q, uint32_t v);
int32_t h_slippi_command(uint8_t command, const uint8_t* payload, uint32_t payload_size,
                         uint8_t* response, uint32_t response_capacity, uint32_t* response_size);
// source_host_boot.cpp
void log_native_pose_bridge_stats();
uint32_t rollback_engine_frame();
void rollback_log(bool exiting = false);
void rollback_frame_callback();
void finish_tournament_before_exit();
// source_host_console.cpp
void trace_ax_voice(const ax::VoiceTrace& trace);
void trace_ax_frame(const char* line);
uint16_t ax_native_rd16(uint32_t addr);
uint32_t ax_native_rd32(uint32_t addr);
void ax_native_wr16(uint32_t addr, uint16_t value);
void ax_native_wr32(uint32_t addr, uint32_t value);
uint32_t held_buttons();
void h_log(const char* text);
void h_panic(const char* file, int32_t line, const char* message);
uint64_t h_ticks();
uint64_t h_boot_time();
void h_poll();
void native_retrace();
void h_gx_fifo(const uint8_t* data, uint32_t size);
void h_native_render_scope_event(const MuNativeRenderScopeEvent* event);
int32_t h_native_pose_capture_enabled();
void h_native_pose_snapshot(const MuNativePoseSnapshot* snapshot);
uint32_t h_music_volume();
int32_t h_native_owner_tracking();
int32_t h_native_draw_identity();
int32_t h_native_rtx_semantics();
void h_vi_configure(uint32_t w, uint32_t h, uint32_t interlaced);
void h_vi_set_next_framebuffer(void*);
void h_vi_flush();
uint32_t h_vi_retrace_count();
uint32_t h_vi_next_field();
void h_vi_set_black(int32_t);
void h_vi_wait_retrace();
int32_t h_vi_idle_step();
void h_pad_read(MuPadStatus out[4]);
void h_pad_rumble(int32_t port, int32_t on);
void h_ai_init_dma(void* buffer, uint32_t length);
void h_ai_start_dma(int32_t on);
void h_ai_set_sample_rate(uint32_t);
void h_ai_set_stream_volume(int32_t, int32_t);
void h_dsp_mail(uint32_t mail);
uint32_t h_dsp_mail_pending();
void* h_aram_base();
uint32_t h_aram_size();
void h_aram_dma(int32_t to_aram, void* mainmem, uint32_t aram_offset, uint32_t length);
void* h_native_alloc(uint32_t size);
void h_native_free(void* ptr);
uint32_t h_mem1_size();
int32_t h_sound_mode();
void h_set_sound_mode(int32_t mode);
int32_t h_progressive_mode();
void h_set_progressive_mode(int32_t);
int32_t h_reset_code();
int32_t h_reset_switch();
void h_stop(int32_t reason, int32_t code);
// source_host_disc.cpp
uint32_t be32(const uint8_t* p);
bool read_fst();
uint32_t fnv1a(const uint8_t* p, size_t n);
void load_system_files(const char* forced_off_by);
int32_t h_disc_entrynum(const char* path);
int32_t h_disc_file(int32_t entrynum, uint32_t* start, uint32_t* length);
void index_music_paths();
bool jukebox_music_pack_read(uint32_t offset, std::filesystem::path* out);
bool jukebox_disc_read(uint32_t offset, void* dst, uint32_t size);
void h_disc_read(uint32_t offset, void* dst, uint32_t size, MuDiscDone done, void* user);
int32_t h_disc_status();
uint32_t h_disc_id(void* out, uint32_t size);
bool music_play_command(const uint8_t* p);
// source_host_mod_overlay.cpp
bool pack_costume_takes_skins(uint32_t entry);
bool read_pack_online(uint32_t offset, void* dst, uint32_t size, bool* ok);
void load_mod_overlay();
bool read_overlay(uint32_t offset, void* dst, uint32_t size, bool* ok);
uint32_t h_mod_flags();
bool maker_cache_command(std::vector<uint8_t>& reply);
// source_host_cosmetics.cpp
void load_cosmetics(bool always_vanilla_unsafe);
bool read_cosmetic(uint32_t offset, void* dst, uint32_t size, bool* ok);
void republish_from_mods_menu();
void plan_random_stage_skin(const std::string& file, std::vector<uint8_t>& reply);
void cycle_stage_skin(const std::string& file, int direction, std::vector<uint8_t>& reply);
void cycle_costume_skin(int character, int costume, int direction, std::vector<uint8_t>& reply,
                        uint32_t allowed_costumes = 0);
// source_host_menu_mods.cpp
bool read_menu_mod(uint32_t offset, void* dst, uint32_t size, bool* ok);
void publish_menu_mods(bool off);
// source_host_new_stages.cpp
bool read_new_stage(uint32_t offset, void* dst, uint32_t size, bool* ok);
bool plan_new_stage(const std::string& stage_path);
void new_stages_command(const uint8_t* p, size_t n, std::vector<uint8_t>& reply);
// source_host_roster.cpp
bool roster_served();
std::filesystem::path installed_fighters_folder();
#ifdef _WIN32
std::filesystem::path gated_plugin_folder(bool ask);
#endif
void publish_roster();
bool read_roster_file(uint32_t offset, void* dst, uint32_t size, bool* ok);
// source_host_mods_menu.cpp
void put_text(std::vector<uint8_t>& out, const std::string& text, size_t width);
void mod_browser_command(const uint8_t* p, size_t n, std::vector<uint8_t>& reply);
// source_host_lobbies.cpp
void lobby_command(const uint8_t* p, size_t n, std::vector<uint8_t>& reply);
// source_host_online.cpp
slippi::online::LocalBuild content_build_for_mode(int mode);
void apply_content_mode(int mode);
void plain_opponent(bool plain, const char* why);
void prepare_practice_content(int mode, bool activate);
int32_t h_online_test_match(MuOnlineMatch* out);
bool native_online_mode();
bool online_option_command(const uint8_t* p, std::vector<uint8_t>& reply);
bool online_content_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply);
// source_host_savestates.cpp
void begin_native_savestates(int slots = slippi::ROLLBACK_MAX_FRAMES);
void note_disc_load(void* dst, uint32_t size, uint32_t offset);
void native_capture(int32_t frame);
void native_load(int32_t frame, const uint8_t* payload);
void report_selftest(int32_t frame, std::vector<uint8_t>& reply);
void h_resim_phase(int32_t entering);
void online_inputs_command(const uint8_t* p);
// source_host_practice.cpp
bool read_training_file(uint32_t offset, void* dst, uint32_t size, bool* ok);
void lab_save();
void lab_load();
bool practice_cpu_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply);
bool cpu_ai_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply);
// source_host_replays.cpp (both builds; the build without Slippi answers that there is no replay)
uint8_t replay_gate(int32_t frame, uint8_t phase);
void write_replay_recording(bool in_background = false);
void drop_replay_recording();
const MuReplayStart* h_replay_start();
void h_replay_frame(int32_t frame, MuReplayFrame* out);
int32_t h_replay_stock_steal(int32_t frame, int32_t port);
void h_replay_event(uint8_t command, const uint8_t* payload, uint32_t size);
void h_replay_finished();
bool replay_defense_command(std::vector<uint8_t>& reply);
bool frame_event_command(const uint8_t* p, uint32_t n);
// source_host_card.cpp
int32_t h_card_probe(int32_t chan, int32_t* mem_size, int32_t* sector_size);
int32_t h_card_mount(int32_t chan);
int32_t h_card_unmount(int32_t chan);
int32_t h_card_open(int32_t chan, const char* name, int32_t* file_no, uint32_t* length);
int32_t h_card_close(int32_t chan, int32_t);
int32_t h_card_create(int32_t chan, const char* name, uint32_t size, int32_t* file_no);
int32_t h_card_delete(int32_t chan, const char* name);
int32_t h_card_rename(int32_t chan, const char* old_name, const char* new_name);
int32_t h_card_read(int32_t chan, int32_t file_no, void* dst, uint32_t length, uint32_t offset);
int32_t h_card_write(int32_t chan, int32_t file_no, const void* src, uint32_t length, uint32_t offset);
int32_t h_card_stat(int32_t chan, int32_t file_no, void* stat, uint32_t stat_size);
int32_t h_card_set_stat(int32_t chan, int32_t file_no, const void* stat, uint32_t stat_size);
int32_t h_card_free_blocks(int32_t chan, int32_t* bytes_unused, int32_t* files_unused);
int32_t h_card_format(int32_t chan);
// source_host_options.cpp
uint32_t h_hud_scales();
uint32_t h_display_options();
uint32_t h_shield_color(int32_t slot);
float h_screen_shake_scale();
uint32_t h_grid_color();
void h_hud_combo(int32_t slot, int32_t link_slot, float percent, int32_t stunned);
void h_hud_extras(int32_t slot, int32_t nana_damage, int32_t team);
void h_hud_player(int32_t slot, int32_t present, int32_t damage, int32_t stocks, float tag_x, float tag_y,
                  int32_t tag_visible);
uint32_t te_words_mask1();
uint32_t h_game_options2();
float h_picture_widen();
uint32_t h_audio_options();
uint32_t h_game_options3();
uint32_t h_game_options();
bool te_host_feature(uint32_t feature);
bool zelda_as_sheik_command(std::vector<uint8_t>& reply);
bool te_menu_music_command(const uint8_t* p, std::vector<uint8_t>& reply);
bool te_settings_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply);

// ---- game code runs inside guarded: an ExitRequested ends the process there ("stopping", source_host_boot.cpp) ----

template <class F> void guarded(F&& f) {
  try { f(); } catch (const ExitRequested& stop) {
    g_exit_code = stop.code;
    finish_tournament_before_exit();
    write_replay_recording();
#ifndef MELEE_NO_SLIPPI
    slippi::spectate::stop();
    host::lobby_directory::shutdown();   // a hosted lobby closed, a join given up
#endif
    rollback_log(true);
    log_native_pose_bridge_stats();
#ifdef __APPLE__
    if (g_mac_running) std::longjmp(g_mac_run_boundary, 1);
#endif
    if (g_shutdown) g_shutdown(stop.code);
#ifdef _WIN32
    plugin_gate::end_run();   // this run's plugin copies (plugin_gate.h)
#endif
    host::end_process(stop.code);   // everything is saved: do not run the libraries' unload code
  }
}

}  // namespace host_internal


using namespace host_internal;   // as the one file's unnamed namespace was: its names are the host's own

// ---- source_port's own functions one file defines and another calls (not in source_host.h) ----
// source_host_practice.cpp
int32_t console_practice_bridge(int32_t op, int32_t* args, int32_t count);
void publish_training_files(const std::filesystem::path& game_folder);
// source_host_replays.cpp
void check_replay_content();
#ifndef MELEE_NO_SLIPPI
extern std::string g_replay_mod_profile;
#endif

}  // namespace source_port
