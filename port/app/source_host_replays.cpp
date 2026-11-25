// Source Port host: Slippi replays. Playback (--replay), the recording of every match, the replay
// viewer's controls, the coach's frame events, a replay's options and content.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

#ifdef MELEE_NO_SLIPPI
// No replay playback and no recording in this build. The game is told there is no replay; the
// callbacks stay in the host table so the same game interface version is served.
uint8_t replay_gate(int32_t, uint8_t) { return 0; }
void write_replay_recording(bool) {}
void drop_replay_recording() {}
const MuReplayStart* h_replay_start() { return nullptr; }
void h_replay_frame(int32_t, MuReplayFrame* out) { *out = MuReplayFrame{}; out->result = MU_REPLAY_TERMINATE; }
int32_t h_replay_stock_steal(int32_t, int32_t) { return 0; }
void h_replay_event(uint8_t, const uint8_t*, uint32_t) {}
void h_replay_finished() {}
#else
// ---- --replay: Slippi replay playback (host API version 9) ----
// The host side of Slippi's playback: what Dolphin's CEXISlippi answers the playback codes over EXI
// (prepareGameInfo, prepareFrameData, prepareIsStockSteal), in the Legacy port's "normal" mode with
// rollback display off, so the final version of every frame plays. The game side is
// sourceport/game/shim/mu_replay.c. The played-back game is recorded again from the game's own
// pre-frame and post-frame events and written as a .slp to --replay-dir.

// ---- the replay viewer's controls (host/replay_control.h) ----
// The game calls in at the top of every engine frame of the replay's match (mu_replay_frame_begin):
// that is where a pause holds and where speed is paced. Jumping back uses the rollback snapshot
// engine: a state is kept every few seconds at that same point, and a jump restores the newest one
// at or before its target and runs forward from it, unseen and unpaced. The recording of the
// played-back game is put back to where it stood at that state, so frames played again are
// recorded once.

void replay_keep(int32_t frame) {
  if (!g_savestates.active()) begin_native_savestates(replay_control::kMaxKeptStates);
  g_savestates.capture(frame);
  g_replay_marks[frame] = g_replay_stream.mark();
}

int32_t replay_restore(int32_t target) {
  int32_t frame = 0;
  if (!g_savestates.active() || !g_savestates.newest_at_or_before(target, &frame)) return replay_control::kNoFrame;
  const auto mark = g_replay_marks.find(frame);
  if (mark == g_replay_marks.end() || !g_savestates.load_keep(frame)) return replay_control::kNoFrame;
  g_replay_stream.rewind(mark->second);
  g_replay_marks.erase(std::next(mark), g_replay_marks.end());
  gx::mark_discontinuity();   // the next frame shown continues from this older state
  return frame;
}

// MU_CMD_REPLAY_GATE. Phase 0, every frame: 1 when the viewer wants to go back (the game then calls
// again with phase 1, with its timers held), else 0. Phase 1: the state is restored.
static void replay_hold_for_live_frame(int32_t frame);

uint8_t replay_gate(int32_t frame, uint8_t phase) {
  if (!g_replay) return 0;
  if (phase == 0) {
    replay_hold_for_live_frame(frame);
    // A recording still being written plays as close to live as it can: a viewer that opened late
    // (an arena watcher's window starts after the game does) runs unpaced and unheard until it is
    // within a frame of the newest one, then plays at normal speed again.
    static bool catching_up = false;
    if (!g_replay->IsProcessingComplete() && !replay_control::status().seeking) {
      const int32_t behind = g_replay->GetLatestIndex() - frame;
      // On the clock the players' games log their frames with ("online frame N wall T"): a test
      // compares the two to see how far behind the match a watcher's window shows it.
      if (frame % 30 == 0 && !catching_up) host::log("replay: live frame %d wall %.3f s", frame, host::now_seconds());
      const bool was = catching_up;
      if (behind > 3) catching_up = true;
      else if (behind <= 1) catching_up = false;
      if (catching_up != was) {
        host::options.fast = catching_up;
        host::audio_set_muted(catching_up);
        replay_control::set_catching_up(catching_up);   // the way there is not drawn: the window joins live
        if (!catching_up) host::log("replay: caught up with the live game at frame %d", frame);
      }
    } else if (catching_up) {
      catching_up = false;
      host::options.fast = false;
      host::audio_set_muted(false);
      replay_control::set_catching_up(false);
    }
    return replay_control::gate(frame) == replay_control::Gate::GoBack ? 1 : 0;
  }
  return replay_control::go_back() == replay_control::Gate::Restored ? 2 : 0;
}

// The Game Start event's payload, from the file's raw element (the recording reuses it as is).
std::vector<uint8_t> read_replay_event(const std::string& path, uint8_t wanted) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  static const char tag[] = "raw[$U#l";
  auto it = std::search(d.begin(), d.end(), tag, tag + 8);
  if (it == d.end()) return {};
  size_t i = (size_t)(it - d.begin()) + 8 + 4;
  if (i + 2 > d.size() || d[i] != 0x35) return {};
  if (d[i + 1] < 1 || (d[i + 1] - 1) % 3) return {};
  const size_t count = (d[i + 1] - 1) / 3;
  std::array<int32_t, 256> sizes;
  sizes.fill(-1);
  for (size_t k = 0; k < count; ++k) {
    const size_t e = i + 2 + 3 * k;
    if (e + 3 > d.size()) return {};
    sizes[d[e]] = (uint32_t)d[e + 1] << 8 | d[e + 2];
  }
  const uint32_t raw_size = be32(d.data() + i - 4);
  if (raw_size && raw_size > d.size() - i) return {};
  const size_t end = raw_size ? i + raw_size : d.size();
  for (size_t pos = i + 1 + d[i + 1]; pos < end;) {
    const uint8_t command = d[pos++];
    if (sizes[command] < 0 || (size_t)sizes[command] > end - pos) return {};
    const size_t size = (size_t)sizes[command];
    if (command == wanted) return std::vector<uint8_t>(d.begin() + pos, d.begin() + pos + size);
    pos += size;
  }
  return {};
}

// Which of the replay's gameplay codes the native game knows, by injection address.
uint32_t replay_codes(const Slippi::GameSettings& s, uint8_t* ps_frozen_toggle) {
  struct Known { uint32_t address; uint32_t code; const char* name; };
  // UCF 0.8 (Slippi's list until it moved to 0.84, so replays from 2023 and before) patches the same
  // three places as 0.84's dashback, shield drop and tumble, with different code. These say "a UCF"
  // here; which one is settled after the loop, from the 0.84-only codes and the code sizes.
  constexpr uint32_t kUcfShared = 0x80000000u;
  static const Known known[] = {
      {0x800D65EC, MU_REPLAY_CODE_UCF084, "UCF 0.84 DBOOC SquatRv fix"},
      {0x800C9A44, kUcfShared, "UCF dashback"}, {0x8006B460, MU_REPLAY_CODE_UCF084, "UCF pad buffer"},
      {0x8008E54C, MU_REPLAY_CODE_UCF084, "UCF SDI"}, {0x8009A0B8, MU_REPLAY_CODE_UCF084, "UCF shield drop extended"},
      {0x800998A4, kUcfShared, "UCF shield drop"}, {0x80093294, MU_REPLAY_CODE_UCF084, "UCF shield SDI"},
      {0x800908F4, kUcfShared, "UCF tumble"},
      {0x8016E510, MU_REPLAY_CODE_NEUTRAL_SPAWN, "NeutralSpawn"},
      {0x801239A8, MU_REPLAY_CODE_FREEZE_GLITCH, "FreezeGlitchFix"},
      {0x801C154C, MU_REPLAY_CODE_INIT_STAGE_DATA, "Init Stage Data"},
      {0x80068EEC, MU_REPLAY_CODE_INIT_PLAYER_DATA, "Init Player Data"},
      {0x8006A880, MU_REPLAY_CODE_OFFSCREEN_DAMAGE, "BrawlOffscreenDamage"},
      {0x800D4C1C, MU_REPLAY_CODE_DEAD_UP_FALL, "FreezeDeadUpFallPhysics init"},
      {0x800D4D68, MU_REPLAY_CODE_DEAD_UP_FALL, "FreezeDeadUpFallPhysics fall"},
      {0x80080E80, MU_REPLAY_CODE_DEAD_UP_FALL, "FreezeDeadUpFallPhysics model"},
      {0x8021AAE4, MU_REPLAY_CODE_FD_BG_SEED, "FD DesyncProofBGTransformations"},
      {0x801C65C8, MU_REPLAY_CODE_PS_ZERO_BUFFER, "Stadium CustomZeroBuffer"},
      {0x801D4760, MU_REPLAY_CODE_PS_IS_VALID, "Stadium GrPsxIsValid"},
      {0x801D457C, MU_REPLAY_CODE_PS_FROZEN_CHECK, "Stadium IngameCheckIfFrozen"},
      // The older online lists (netplay-old1, 2023): Stadium always frozen, a jump from the start of
      // grStadium_801D4548 to its exit with no toggle of its own.
      {0x801D4578, MU_REPLAY_CODE_PS_FROZEN_CHECK, "Stadium frozen (older online lists)"},
      // Preload Stadium Transformations (Slippi's 2023 core list): the next transformation is rolled
      // and loaded as the countdown starts, not when it ends (grpstadium.c, MU_RC_PS_PRELOAD).
      {0x801D14C8, MU_REPLAY_CODE_PS_PRELOAD, "Stadium preload: init isLoaded"},
      {0x801D45EC, MU_REPLAY_CODE_PS_PRELOAD, "Stadium preload: load transformation"},
      {0x801D460C, MU_REPLAY_CODE_PS_PRELOAD, "Stadium preload: preloaded transition"},
      {0x801D4610, MU_REPLAY_CODE_PS_PRELOAD, "Stadium preload: skip normal decision"},
      {0x801D4724, MU_REPLAY_CODE_PS_PRELOAD, "Stadium preload: skip normal load"},
      {0x801D4F14, MU_REPLAY_CODE_PS_PRELOAD, "Stadium preload: reset isLoaded"},
      {0x800165AC, MU_REPLAY_CODE_PS_FILE_LOAD, "Stadium StadiumFileLoad"},
      // Slippi 3.7.0: two stage states that carried over from the last game are reset at stage init.
      {0x801CD37C, MU_REPLAY_CODE_CASTLE_WIND_RESET, "Reset Castle Wind"},
      {0x801D69B0, MU_REPLAY_CODE_KONGO_LOG_RESET, "Reset Kongo Log Bridge"},
      {0x8008653C, MU_REPLAY_CODE_WHISPY_FIX, "WhispyBlowDirFix"},
      {0x800AC5B8, MU_REPLAY_CODE_NANA_DETERMINISM, "NanaDeterminism"},
      {0x801D24FC, MU_REPLAY_CODE_PS_MONITOR, "PSCameraIndependentMonitor"},
      {0x800DB880, MU_REPLAY_CODE_PREVENT_WOBBLING, "PreventWobbling air"},
      {0x800DBBD4, MU_REPLAY_CODE_PREVENT_WOBBLING, "PreventWobbling ground"},
      {0x8008F090, MU_REPLAY_CODE_PREVENT_WOBBLING, "PreventWobbling check"},
      // Not gameplay: results screen panels, the online static data table, the HUD stock display.
      {0x80179378, 0, nullptr}, {0x8017900C, 0, nullptr}, {0x8017A890, 0, nullptr},
      {0x8000568C, 0, nullptr}, {0x802F84C8, 0, nullptr},
  };
  // The injection lists that say which codes affect gameplay ship with the playback Sys folder.
  std::string sys = host::options.sys_dir;
  for (const char* candidate : {host::options.sys_dir.c_str(), "port/slippi_sys_playback", "SysPlayback"}) {
    if (std::filesystem::exists(std::filesystem::path(candidate) / "Slippi" / "InjectionLists")) { sys = candidate; break; }
  }
  size_t dropped = 0;
  auto kept = slippi::playback::gameplay_codes(s.geckoCodes, sys, &dropped);
  uint32_t codes = 0;
  size_t unknown = 0;
  size_t ucf_dashback_bytes = 0, ucf_shield_drop_bytes = 0;
  for (const auto& c : kept) {
    const Known* k = nullptr;
    for (const auto& e : known) if (e.address == c.address) { k = &e; break; }
    if (!k) {
      ++unknown;
      host::log("replay: code %08X (type %02X, %zu bytes) is not implemented natively; this replay may diverge",
                c.address, c.type, c.bytes.size());
      continue;
    }
    codes |= k->code;
    if (c.address == 0x800C9A44) ucf_dashback_bytes = c.bytes.size();
    if (c.address == 0x800998A4) ucf_shield_drop_bytes = c.bytes.size();
    // IngameCheckIfFrozen keeps its toggle in its own code: b; blrl; .byte toggle.
    if (c.address == 0x801D457C && c.bytes.size() >= 8 + 12) *ps_frozen_toggle = c.bytes[8 + 8];
    // That jump (32 bytes: fmr f31,f1; lis/ori r12,801D4FD8; mtctr; bctr): no toggle, always frozen.
    if (c.address == 0x801D4578 && c.bytes.size() == 32 && c.bytes[0x18] == 0x4E && c.bytes[0x1B] == 0x20) *ps_frozen_toggle = 1;
  }
  // Which UCF the shared sites belong to. 0.84 brings codes of its own (pad buffer, SDI, ...); 0.8's
  // dashback and shield drop are 352 and 312 bytes (External/UCF 0.8, Slippi's list_console_UCF).
  // Anything else is a UCF the native game does not have: said, and the replay plays without one.
  if (codes & kUcfShared) {
    codes &= ~kUcfShared;
    if (codes & MU_REPLAY_CODE_UCF084) {
      // 0.84: the shared sites are its own.
    } else if (ucf_dashback_bytes == 352 && ucf_shield_drop_bytes == 312) {
      codes |= MU_REPLAY_CODE_UCF08;
      host::log("replay: recorded with UCF 0.8 (dashback, shield drop, tumble); played with UCF 0.8");
    } else {
      codes |= MU_REPLAY_CODE_UCF_OTHER;
      ++unknown;
      host::log("replay: recorded with a UCF other than 0.84 or 0.8 (dashback %zu bytes, shield drop %zu bytes); "
                "this replay may diverge", ucf_dashback_bytes, ucf_shield_drop_bytes);
    }
  }
  host::log("replay: code list %zu bytes, %zu gameplay codes kept (%zu denylisted by %s), %zu not native; codes mask %08X",
            s.geckoCodes.size(), kept.size(), dropped, sys.c_str(), unknown, codes);
  return codes;
}

// in_background (the match's end, at GAME!): the file is written on a thread of its own, so the game
// never waits on the disk there. A 4 to 8 MB replay took players 120 ms to 8.5 s to save (the file
// checked by antivirus as it closes), and an online opponent's game gave up on the stalled one.
// Everything else (quitting, a crash, a recording not written yet) writes at once, after any write still
// going (host::wait_file_writes; end_process waits for it too).
void write_replay_recording(bool in_background) {
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  if (g_replay_squad && !g_replay_written && g_replay_stream.active()) {
    g_replay_written = true; // handled: later flushes must not log this recording again
    host::log("replay: not kept: Squad Strike swaps cannot be played back yet");
    return;
  }
#endif
  // Written already (the menu after GAME!, quitting): nothing to wait for here. end_process waits.
  if (g_replay_written || !g_replay_stream.active() || g_replay_spectator_only) return;
  // The replay viewer saves only a game the player took part in (a takeover); a replay only watched is
  // already on disk, and saving it again left a copy per watch (replay_control::took_over).
  // MELEE_REPLAY_SAVE_WATCHED=1 keeps the copy: tools/replay_compare.py checks the viewer against it.
  static const bool save_watched = [] { const char* v = host::test_env::getenv("MELEE_REPLAY_SAVE_WATCHED"); return v && *v == '1'; }();
  if (g_replaying && !replay_control::took_over() && !save_watched) {
    g_replay_written = true;
    host::log("replay: watched only, no copy saved");
    return;
  }
  host::wait_file_writes();   // an earlier match's file is on disk before this one is named
  std::vector<uint8_t> file = g_replay_stream.encode();
  if (file.empty()) {
    host::log("replay: recording failed: %s", g_replay_stream.error().c_str());
    return;
  }
  // Named by the game's start, as Dolphin names the file when the game creates it.
  std::time_t now = g_replay_stream.start_time() ? (std::time_t)g_replay_stream.start_time() : std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  char name[64];
  std::strftime(name, sizeof name, "Game_%Y%m%dT%H%M%S.slp", &tm);
  // Slippi's monthly subfolders (--replay-month-folders): the month the game started, as Dolphin picks
  // the folder when the game creates its file.
  std::string folder = host::options.replay_dir;
  if (host::options.replay_month_folders) {
    char month[16];
    std::strftime(month, sizeof month, "%Y-%m", &tm);
    folder = (std::filesystem::path(folder) / month).string();
  }
  // This recording is taken from here: a later call (the menu, quitting) does not write it again.
  g_replay_written = true;
  auto write = [file = std::move(file), dir = std::move(folder), name = std::string(name)]() {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Exclusive creation prevents quick successive matches (or two clients) from
    // overwriting a recording with the same second-resolution timestamp.
    std::filesystem::path out = std::filesystem::path(dir) / name;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
      if (suffix) out = std::filesystem::path(dir) / (name.substr(0, name.size() - 4) + "_" + std::to_string(suffix) + ".slp");
      handle = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (handle != INVALID_HANDLE_VALUE) {
      const auto began = std::chrono::steady_clock::now();
      // Tests: MELEE_TEST_REPLAY_WRITE_DELAY_MS=N makes the disk this slow (a match must not wait on it).
      if (const char* slow = host::test_env::getenv("MELEE_TEST_REPLAY_WRITE_DELAY_MS"))
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(slow)));
      DWORD written = 0;
      const bool ok = WriteFile(handle, file.data(), (DWORD)file.size(), &written, nullptr) && written == file.size();
      const bool closed = CloseHandle(handle) != 0;
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
      host::log("replay: %s %s (%zu bytes, %.0f ms)", ok && closed ? "recording written to" : "write failed for",
                out.string().c_str(), file.size(), ms);
      if (ok && closed) net_trace::replay_saved(out.string().c_str());
    } else {
      host::log("replay: cannot write %s (error %lu)", out.string().c_str(), GetLastError());
    }
  };
  if (!in_background) { write(); return; }
  host::file_write_begin();
  std::thread([write = std::move(write)]() mutable {
    write();
    host::file_write_end();
  }).detach();
}

// The replay being played was refused before its match (h_stop): the recording begun from its Game
// Start would hold no frames, so it is not written.
void drop_replay_recording() {
  if (g_replay_written || !g_replay_stream.active()) return;
  g_replay_written = true;
  host::log("replay: no recording written: the match was not started");
}

const MuReplayStart* h_replay_start() { return g_replay ? &g_replay_start : nullptr; }

// A recording that was never finished (the game or Dolphin closed in the middle of the match, so the
// file has no Game End and no metadata) gets no more frames. The game does not stop on a frame it
// cannot fetch: the match went on with nobody pressing anything until its timer ran out, or for good
// in a match without one. Such a file counts as over when it has not changed for a few seconds (its
// write time on the first look: a file from an earlier session ends at its last frame at once) and
// no other program holds it open for writing. A live broadcast keeps growing, and its writer keeps
// it open even while the match is paused, so it is still waited for.
static bool replay_recording_abandoned() {
  static std::string for_path;
  static bool abandoned = false;
  static std::uintmax_t last_size = 0;
  static double last_growth = 0.0, last_look = -1.0;
  const double now = host::now_seconds();
  if (for_path != g_replay_path) {   // a new file: forget the last one's observations
    for_path = g_replay_path; abandoned = false; last_size = ~std::uintmax_t{0}; last_look = -1.0;
  }
  if (abandoned) return true;
  if (last_look >= 0.0 && now - last_look < 0.5) return false;   // asked again every retrace while waiting
  last_look = now;
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::u8path(g_replay_path);
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return false;
  if (size != last_size) {
    const bool first = last_size == ~std::uintmax_t{0};
    last_size = size;
    last_growth = now;
    if (!first) return false;                      // it grew while we watched: still being written
    const auto written = std::filesystem::last_write_time(path, ec);
    if (!ec) {
      const double age = std::chrono::duration<double>(std::filesystem::file_time_type::clock::now() - written).count();
      if (age > 0.0) last_growth = now - age;
    }
  }
  if (now - last_growth < 3.0) return false;
#ifdef _WIN32
  // Opening it for writing while only allowing others to read fails with a sharing violation exactly
  // when another handle can write it. Nothing is written; the handle is closed at once.
  HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return abandoned = true; }
  if (GetLastError() == ERROR_SHARING_VIOLATION) return false;
#endif
  return abandoned = now - last_growth >= 30.0;   // no way to ask who writes it (read-only file, other systems)
}

// WAIT for a frame that has not arrived yet, unless the recording was abandoned (above).
static int32_t replay_wait_or_end(int32_t frame) {
  if (!replay_recording_abandoned()) return MU_REPLAY_WAIT;
  host::log("replay: %s stops at frame %d with no Game End and nothing writing it any more; playback ends there",
            g_replay_path.c_str(), frame - 1);
  return MU_REPLAY_TERMINATE;
}

// A frame of a recording still being written that can be played: there, all of its inputs in, and
// final. A finished recording answers yes for every frame (the fetch then finds it or ends).
static bool replay_frame_ready(int32_t frame) {
  const bool exists = g_replay->DoesFrameExist(frame);
  if (g_replay->IsProcessingComplete()) return true;
  if (!exists || !g_replay->GetFrame(frame)->inputsFullyFetched) return false;
  const auto v = g_replay->GetVersion();
  const bool has_finalized = v[0] > 3 || (v[0] == 3 && v[1] >= 7);
  return !has_finalized || g_replay->GetLastFinalizedFrame() >= frame;
}

// Top of a frame, where a pause holds (replay_gate): a live recording's next frame is waited for
// here, the way Dolphin's mirror mode stops the game until the frame arrives. The fetch would
// otherwise answer WAIT and the match would run that frame with no one's inputs, and every frame
// after it would be one behind (an arena watcher's viewer that caught up with the live game).
static void replay_hold_for_live_frame(int32_t frame) {
  if (replay_frame_ready(frame)) return;
  const double started = host::now_seconds();
  bool told = false;
  while (!host::exit_requested() && !replay_frame_ready(frame) && !replay_recording_abandoned()) {
    if (!told && host::now_seconds() - started > 1.0) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(std::filesystem::u8path(g_replay_path), ec);
      host::log("replay: waiting for frame %d of %s (read to frame %d, %llu bytes on disk)", frame,
                g_replay_path.c_str(), g_replay->GetLatestIndex(), ec ? 0ull : (unsigned long long)size);
      told = true;
    }
    if (host::g_has_window) host::window_pump();   // the simulation thread owns the window only without the render thread
    { host::perf::Scope wait_cost(host::perf::Stage::Wait); Sleep(4); }
  }
  if (told) host::log("replay: frame %d came after %.1f s", frame, host::now_seconds() - started);
}

void h_replay_frame(int32_t frame, MuReplayFrame* out) {
  *out = MuReplayFrame{};
  if (!g_replay) { out->result = MU_REPLAY_TERMINATE; return; }
  // A replay still being written (a live broadcast the Slippi Launcher saves as it arrives) has no
  // Game End yet: a frame that is not there, not fully received or not yet finalized is waited for,
  // as Dolphin's mirror mode does. Once Game End is read, a missing frame ends playback.
  const bool complete = g_replay->IsProcessingComplete();
  if (!g_replay->DoesFrameExist(frame)) { out->result = complete ? MU_REPLAY_TERMINATE : replay_wait_or_end(frame); return; }
  Slippi::FrameData* f = g_replay->GetFrame(frame);
  if (!complete) {
    const auto v = g_replay->GetVersion();
    const bool has_finalized = v[0] > 3 || (v[0] == 3 && v[1] >= 7);
    if (!f->inputsFullyFetched || (has_finalized && g_replay->GetLastFinalizedFrame() < frame)) {
      out->result = replay_wait_or_end(frame);
      return;
    }
  }
  out->result = MU_REPLAY_CONTINUE;
  {
    const int live = replay_control::takeover_port();
    out->live_ports = live >= 0 && live < 4 ? 1u << live : 0u;
  }
  out->seed_exists = f->randomSeedExists ? 1 : 0;
  out->seed = f->randomSeed;
  for (uint8_t port = 0; port < 4; ++port) {
    for (int follower = 0; follower < 2; ++follower) {
      const auto& source = follower ? f->followers : f->players;
      auto it = source.find(port);
      if (it == source.end()) continue;
      const Slippi::PlayerFrameData& d = it->second;
      MuReplayCharacter& c = out->character[port][follower];
      c.present = 1;
      c.random_seed = d.randomSeed;
      c.joystick_x = d.joystickX; c.joystick_y = d.joystickY; c.cstick_x = d.cstickX; c.cstick_y = d.cstickY;
      c.trigger = d.trigger;
      c.buttons = d.buttons;
      c.x = d.locationX; c.y = d.locationY; c.facing = d.facingDirection;
      c.action_state = d.animation;
      c.percent = d.percent;
      c.joystick_x_raw = d.joystickXRaw; c.joystick_y_raw = d.joystickYRaw;
      c.cstick_x_raw = d.cstickXRaw; c.cstick_y_raw = d.cstickYRaw;
    }
  }
}

int32_t h_replay_stock_steal(int32_t frame, int32_t port) {
  if (!g_replay || !g_replay->DoesFrameExist(frame)) return 0;
  return g_replay->GetFrame(frame)->players.count((uint8_t)port) ? 1 : 0;
}

void h_replay_event(uint8_t command, const uint8_t* payload, uint32_t size) {
  if (!g_replay && command == 0x36) {
    write_replay_recording();
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    // Save/skip the PREVIOUS recording before replacing its policy with this match's.
    g_replay_squad = g_squad_replay_pending;
    g_squad_replay_pending = false;
    // Squad Strike in Direct: kept, with the setup it was played with (set after begin_slippi below).
    const std::vector<uint8_t> squad_setup = std::move(g_squad_replay_setup_pending);
    g_squad_replay_setup_pending.clear();
#endif
    // Game Start payload offsets (the payload has no command byte): 0x1A3 major scene, 0x2BD the
    // online match id. The game fills the names, codes, match id, game and tiebreak numbers in.
    const std::vector<uint8_t> start(payload, payload + size);
    const bool online = size >= 0x2F8 && (start[0x1A3] == 0x08 || start[0x2BD] != 0);
    std::vector<uint8_t> codes;
    if (online) {
      codes.assign(std::begin(native_online_recording_codes), std::end(native_online_recording_codes));
    } else if (!host::options.vanilla_game) {
      codes.assign(std::begin(native_recording_codes), std::end(native_recording_codes));
    } else {
      codes = {0xFF, 0, 0, 0, 0, 0, 0, 0};
    }
    g_replay_written = false;
    {
      const char* record = std::getenv("MELEE_SOURCE_RECORD");
      g_replay_spectator_only = !online && record && std::string(record) == "2";
    }
    // Only layers that change the disc's files make a replay need them (a save file alone does not).
    g_replay_stream.set_mod_profile(g_retail_view || g_view_alias.empty() ? std::string() : g_mod_fingerprint);
    // The 20XX TE features in effect (offline only; online matches never carry them).
    // (Or the Hack Pack's share of them, with the pack and no TE save: te_words_mask1/2.)
    // What RAN, not what is saved: a --vanilla-only session plays with none of them whatever the
    // Game tab holds, and its replay must say so, or playback would switch "20XX CPUs" back on and
    // a Vanilla launch would refuse the recording as modded.
    g_replay_stream.set_feature_options(online || host::gameplay::vanilla_only() ? 0u
                                            : (gx::RenderOptions::live_te_options() & te_words_mask1()));
    g_replay_stream.set_feature_options2(online ? 0u : h_game_options2());   // "20XX CPUs" too: playback replays it
    g_replay_stream.set_feature_options3(online ? 0u : h_game_options3());   // CPU training options, likewise
    // The CPU AI picker's choices this match ran with (command 0xE7 op 2, sent before this event).
    g_replay_stream.set_cpu_ai(online || host::gameplay::vanilla_only() ? 0u : g_cpu_ai_match);
    // Added fighters online: which package each port played, by content hash (the ids are local).
    g_replay_stream.set_nucleus_fighters(online ? host::online_fighters::match_fighters()
                                                : std::map<uint8_t, std::string>{});
    // Online: each player's display name and connect code in the metadata (players.<port>.names),
    // as Dolphin writes them; offline none, as Dolphin.
    {
      std::map<uint8_t, slippi::NativeReplayStream::PlayerName> names;
#ifndef MU_NO_SLIPPI
      if (online) {
        const auto players = slippi::online::recording_names();
        for (uint8_t port = 0; port < 4; ++port)
          if (!players[port].display_name.empty() || !players[port].connect_code.empty())
            names[port] = {players[port].display_name, players[port].connect_code};
      }
#endif
      g_replay_stream.set_player_names(std::move(names));
    }
    // The spectator server (Slippi Dolphin's): start_game, then the command table and every event
    // as recorded, then end_game after Game End, the order Dolphin's DMAWrite sends them.
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    if (g_replay_squad || !squad_setup.empty()) {
      g_replay_stream.set_event_sink(nullptr);
      host::log("spectate: not streamed: Squad Strike swaps cannot be watched yet");
    } else {
#endif
    slippi::spectate::start_game();
    g_replay_stream.set_event_sink([](const uint8_t* data, uint32_t size) {
#ifndef MU_NO_SLIPPI
      slippi::online::record_report_event(data, size);
#endif
      slippi::spectate::write(data, size);
    });
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    }
#endif
    if (!g_replay_stream.begin_slippi(start, codes, (int64_t)std::time(nullptr))) {
      host::log("replay: cannot start native recording: %s", g_replay_stream.error().c_str());
#if MU_SQUAD
      // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
      if (!g_replay_squad && squad_setup.empty())
#endif
      slippi::spectate::end_game();
    }
    else if (online)
      host::log("replay: recording online game %u (tiebreak %u)", read_be32(start.data() + 0x2F0),
                read_be32(start.data() + 0x2F4));
#if MU_SQUAD
    g_replay_stream.set_squad_setup(squad_setup);   // "muSquad", for this recording only
    if (!squad_setup.empty()) host::log("replay: Squad Strike in Direct, its setup kept in the replay (%zu bytes)", squad_setup.size());
#endif
    return;
  }
  // Dolphin closes the file at the Game End event; what the game still sends is not recorded.
  if (!g_replay && (!g_replay_stream.active() || g_replay_stream.ended())) return;
  if (!g_replay_stream.append(command, payload, size))
    host::log("replay: cannot record event %02X: %s", command, g_replay_stream.error().c_str());
  if (!g_replay && command == 0x39) {
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    if (!g_replay_squad && g_replay_stream.squad_setup().empty())   // a squad match was never streamed
#endif
    slippi::spectate::end_game();
    write_replay_recording(true);   // at GAME!: the disk is not waited on
  }
}

void h_replay_finished() {
  if (!g_replay) {
    write_replay_recording();
    return; // An ordinary VS match returns to the menu, ready to record the next match.
  }
  host::log("replay: playback finished");
  if (g_savestates.active() && !g_replay_marks.empty())
    host::log("replay: %zu states were kept for jumping back (%zu MB)", g_savestates.kept(), g_savestates.pool_bytes() >> 20);
  replay_control::end();
  if (g_replay_game_end.empty()) g_replay_game_end = read_replay_event(g_replay_path, 0x39);   // a live file gets it last
  if (!g_replay_game_end.empty() && g_replay_stream.last_frame() >= g_replay->GetLatestIndex())
    h_replay_event(0x39, g_replay_game_end.data(), (uint32_t)g_replay_game_end.size());
  write_replay_recording();
  host::set_replay_viewing(false);   // played to its end: a launcher may start the next queued replay
  host::request_exit(0);
}
#endif

static std::array<int, 6> last_logged_defense;

// MU_CMD_REPLAY_DEFENSE, from h_slippi_command (source_host.cpp).
bool replay_defense_command(std::vector<uint8_t>& reply) {
  // The Opponent setting (gx/practice_menu.h): smart defense, no DI or techs, the Practice CPU's own
  // settings, or nothing (follow the recording).
  const int defense = replay_control::defense();
  if (defense == 3) {
    last_logged_defense.fill(-1);
    return true;
  }
  MuPcConfig cfg = practice_cpu::config();
  if (defense == 0) {
    // Smart: every defensive option (shield, dodges, out of shield, DI and SDI, techs, getups,
    // escaping after hitstun, the ledge), and it never starts an attack in neutral.
    mu_pc_defaults(&cfg);
    cfg.aggression = 0;
    for (auto& v : cfg.move_odds) v = 0;
    cfg.move_odds[MU_PC_MOVE_STAND] = 100;
    cfg.sdi_amount = 2;
  } else if (defense == 1) {
    mu_pc_preset(1, &cfg);   // stands still and only reacts to being hit
    for (auto& v : cfg.di_odds) v = 0;
    cfg.di_odds[MU_PC_DI_NONE] = 100;
    for (auto& v : cfg.tech_odds) v = 0;
    cfg.tech_odds[MU_PC_TECH_MISS] = 100;
  }
  cfg.enabled = 1;
  const std::array<int, 6> current = {
      defense, cfg.aggression, cfg.tech_odds[0], cfg.tech_odds[1], cfg.tech_odds[2], cfg.tech_odds[3]};
  if (last_logged_defense != current) {
    last_logged_defense = current;
    host::log("replay: the opponent defends (setting %d, aggression %d, techs %d %d %d %d)", defense, cfg.aggression,
              cfg.tech_odds[0], cfg.tech_odds[1], cfg.tech_odds[2], cfg.tech_odds[3]);
  }
  const auto* bytes = reinterpret_cast<const uint8_t*>(&cfg);
  reply.assign(bytes, bytes + sizeof cfg);
  return true;
}

// MU_CMD_FRAME_EVENT, from h_slippi_command (source_host.cpp).
bool frame_event_command(const uint8_t* p, uint32_t n) {
  replay_coach::Event e;
  if (replay_coach::parse(p, n, e)) {
    replay_coach::add(e);
    static const bool log_events = host::test_env::getenv("MELEE_TEST_COACH_LOG") != nullptr;   // tests
    if (log_events) {
      const replay_coach::Readout r = replay_coach::readout(e);
      host::log("coach: frame %d P%d %s %s %s | %s (move %d)", e.frame, e.actor + 1, r.head.c_str(), r.value.c_str(),
                r.detail.c_str(), replay_coach::advice(e).c_str(), e.move);
    }
  }
  return true;
}

}  // namespace host_internal

#ifdef MELEE_NO_SLIPPI
void check_replay_content() {}
bool set_replay(const char*) { return false; }   // main refuses --replay before it gets here
#else
// --replay <file.slp>: play a Slippi replay and record it again (see h_replay_start above).
// The "modProfile" metadata a native recording made with mod layers carries (see
// NativeReplayStream::set_mod_profile); empty for the retail game and for other recorders.
uint32_t read_replay_feature_options(const char* path, int word = 0) {   // 0 "muOptions", 1 "muOptions2", 2 "muOptions3", 3 "muCpuAi"
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  static const char key1[] = "U\x09muOptionsl";
  static const char key2[] = "U\x0AmuOptions2l";
  static const char key3[] = "U\x0AmuOptions3l";
  static const char key4[] = "U\x07muCpuAil";
  const char* key = word == 3 ? key4 : word == 2 ? key3 : word == 1 ? key2 : key1;
  const size_t key_len = word == 3 ? sizeof key4 - 1 : word == 2 ? sizeof key3 - 1 : word == 1 ? sizeof key2 - 1 : sizeof key1 - 1;
  const auto at = std::search(bytes.begin(), bytes.end(), key, key + key_len);
  if (at == bytes.end() || bytes.end() - at < (std::ptrdiff_t)(key_len + 4)) return 0;
  const uint8_t* p = (const uint8_t*)&*(at + key_len);
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
std::string read_replay_mod_profile(const char* path) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  static const char key[] = "U\x0amodProfileSU\x40";
  const auto at = std::search(bytes.begin(), bytes.end(), key, key + sizeof key - 1);
  if (at == bytes.end() || bytes.end() - at < (std::ptrdiff_t)(sizeof key - 1 + 64)) return {};
  return std::string(at + (sizeof key - 1), at + (sizeof key - 1) + 64);
}

// The first added fighter an online recording names (metadata "nucleusFighters", an object of port ->
// "<content hash hex> <name>", NativeReplayStream::set_nucleus_fighters): its name, printable only;
// empty when the key is absent.
static std::string g_replay_added_fighter;
std::string read_replay_added_fighter(const char* path) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  static const char key[] = "U\x0FnucleusFighters{";
  const auto at = std::search(bytes.begin(), bytes.end(), key, key + sizeof key - 1);
  if (at == bytes.end()) return {};
  const auto byte = [&bytes](size_t k) { return k < bytes.size() ? (int)(uint8_t)bytes[k] : -1; };
  size_t i = (size_t)(at - bytes.begin()) + sizeof key - 1;
  if (byte(i) != 'U' || byte(i + 1) < 0) return "an added fighter";
  i += 2 + (size_t)byte(i + 1);                                  // the port
  if (byte(i) != 'S' || byte(i + 1) != 'U' || byte(i + 2) < 0) return "an added fighter";
  const size_t len = (size_t)byte(i + 2);
  i += 3;
  if (i + len > bytes.size()) return "an added fighter";
  const std::string text(bytes.begin() + i, bytes.begin() + i + len);
  const size_t space = text.find(' ');
  std::string name;
  for (char c : space == std::string::npos ? std::string() : text.substr(space + 1))
    if (c >= 0x20 && c < 0x7F && name.size() < 32) name += c;
  return name.empty() ? "an added fighter" : name;
}

#if MU_SQUAD
// Squad Strike in Direct: the match setup a recording kept ("muSquad", hex of mu_squad_wire.h's
// MU_SQUAD_MATCH_VERSION bytes, NativeReplayStream::set_squad_setup); empty when it has none or it is not
// one this game reads.
std::vector<uint8_t> read_replay_squad_setup(const char* path) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  static const char key[] = "U\x07muSquadSU";
  const auto at = std::search(bytes.begin(), bytes.end(), key, key + sizeof key - 1);
  if (at == bytes.end() || bytes.end() - at < (std::ptrdiff_t)sizeof key) return {};
  const size_t length = (uint8_t)*(at + (sizeof key - 1));
  if (length % 2 || bytes.end() - at < (std::ptrdiff_t)(sizeof key + length)) return {};
  std::vector<uint8_t> out;
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (size_t i = 0; i < length; i += 2) {
    const int hi = nibble(*(at + sizeof key + i)), lo = nibble(*(at + sizeof key + i + 1));
    if (hi < 0 || lo < 0) return {};
    out.push_back((uint8_t)(hi << 4 | lo));
  }
  MuSquadSetup setup{};
  if (!mu_squad_match_decode(out.data(), (unsigned)out.size(), &setup)) {
    host::log("replay: %s has a Squad Strike setup this game cannot read; played as a normal match", path);
    return {};
  }
  return out;
}
#endif

// A replay must play with the content it was recorded with: different fighter or stage files
// play a different game from the same inputs.
void check_replay_content() {
  if (!g_replay) return;
  // Game Start holds the recording machine's own id for an added fighter, and a replay plays without
  // the installed fighters (id 26 is Master Hand there): refused the way the game refuses a replay it
  // cannot play (kReplayRefusedExit, one log line, no recording), until replays map the content hash.
  if (!g_replay_added_fighter.empty()) {
    host::log("replay: %s was played with an added fighter (%s), and replays with added fighters cannot be "
              "played yet; the match is not started", g_replay_path.c_str(), g_replay_added_fighter.c_str());
    drop_replay_recording();
    host::request_exit(host::kReplayRefusedExit);
    return;
  }
  if (!g_replay_mod_profile.empty() && g_replay_mod_profile != g_mod_fingerprint)
    host::die("replay: %s was recorded with mod content %s, but this run has %s; enable the same mods to play it",
              g_replay_path.c_str(), g_replay_mod_profile.substr(0, 16).c_str(),
              g_mod_fingerprint.empty() ? "the retail game" : g_mod_fingerprint.substr(0, 16).c_str());
  if (g_replay_mod_profile.empty() && !g_mod_fingerprint.empty())
    host::log("replay: %s records no mod content but mods are enabled; it may not play back as recorded",
              g_replay_path.c_str());
}

bool set_replay(const char* path) {
  g_replay_mod_profile = read_replay_mod_profile(path);
  g_replay_added_fighter = read_replay_added_fighter(path);
  g_replay_feature_options = read_replay_feature_options(path);
  g_replay_feature_options2 = read_replay_feature_options(path, 1);
  g_replay_feature_options3 = read_replay_feature_options(path, 2);
  g_replay_cpu_ai = read_replay_feature_options(path, 3) & 0xFFFFu;
  bool squads = false;
#if MU_SQUAD
  g_replay_squad_setup = read_replay_squad_setup(path);   // Squad Strike in Direct ("muSquad"), else empty
  squads = !g_replay_squad_setup.empty();
  if (squads) host::log("replay: Squad Strike in Direct, played with its recorded setup (%zu bytes)", g_replay_squad_setup.size());
#endif
  if (host::gameplay::vanilla_only() &&
      (!g_replay_mod_profile.empty() || g_replay_feature_options || g_replay_feature_options2 ||
       (g_replay_feature_options3 & MU_GAME_OPTION3_CPU_ALL) || g_replay_cpu_ai || squads)) {
    std::fprintf(stderr, "--vanilla-only cannot play a replay recorded with gameplay mods; open it in a normal launch\n");
    return false;
  }
  last_logged_defense.fill(-1);
  g_replaying = true;
  if (g_replay_feature_options) host::log("replay: recorded with game options %08X", g_replay_feature_options);
  if (g_replay_cpu_ai) host::log("replay: recorded with CPU AI choices %04X", g_replay_cpu_ai);
  g_replay = Slippi::SlippiGame::FromFile(path);
  replay_coach::clear();
  if (!g_replay || !g_replay->AreSettingsLoaded()) {
    host::log("replay: cannot read %s", path);
    g_replay.reset();
    return false;
  }
  g_replay_path = path;
  g_replay_game_start = read_replay_event(path, 0x36);
  {
    // The coach's tips from the file itself, before anything plays (host/replay_scan.h).
    const double started = host::now_seconds();
    const std::vector<replay_coach::Event> found = replay_scan::scan(*g_replay);
    replay_coach::add_scanned(found);
    host::log("replay: %zu frame data events read from the file (%.0f ms)", found.size(), (host::now_seconds() - started) * 1000.0);
    if (host::test_env::getenv("MELEE_TEST_COACH_LOG")) {   // tests: the file's readings, to compare with the game's
      for (const replay_coach::Event& e : found) {
        if (e.kind != replay_coach::Kind::Shield && e.kind != replay_coach::Kind::Hit) continue;
        const replay_coach::Readout r = replay_coach::readout(e);
        host::log("scan: frame %d P%d %s %s %s (move %d)", e.frame, e.actor + 1, r.head.c_str(), r.value.c_str(), r.detail.c_str(), e.move);
      }
    }
  }
  g_replay_game_end = read_replay_event(path, 0x39);
  if (g_replay_game_start.empty()) { host::log("replay: %s has no Game Start event", path); g_replay.reset(); return false; }
  Slippi::GameSettings* s = g_replay->GetSettings();
  g_replay_stream.set_event_sink(nullptr);   // the viewer's own recording is not broadcast
  replay_control::forget_takeovers();
  if (!g_replay_stream.begin(g_replay_game_start, s->geckoCodes)) {
    host::log("replay: cannot start recording: %s", g_replay_stream.error().c_str());
    g_replay.reset();
    return false;
  }
  g_replay_written = false;
  g_replay_stream.set_mod_profile(g_replay_mod_profile);
#if MU_SQUAD
  g_replay_stream.set_squad_setup(g_replay_squad_setup);   // the viewer's recording plays back the same way
#endif
  std::array<uint32_t, Slippi::GAME_INFO_HEADER_SIZE> header = s->header;
  // Dolphin's prepareGameInfo: a player who is Sheik or Zelda gets that character in the header.
  for (int i = 0; i < 4; ++i) {
    if (!g_replay->DoesPlayerExist((int8_t)i)) continue;
    const uint8_t ext = s->players[(uint8_t)i].characterId;
    if (ext != 0x12 && ext != 0x13) continue;
    header[24 + 9 * i] = (header[24 + 9 * i] & 0x00FFFFFFu) | (uint32_t)ext << 24;
  }
  for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; ++i)
    for (int b = 0; b < 4; ++b) g_replay_start.game_info[4 * i + b] = (uint8_t)(header[i] >> (24 - 8 * b));
  g_replay_start.random_seed = s->randomSeed;
  g_replay_start.is_pal = s->isPAL;
  auto version = g_replay->GetVersion();
  g_replay_start.preload_ps = version[0] > 1 || (version[0] == 1 && version[1] > 2);
  g_replay_start.frozen_ps = s->isFrozenPS;
  g_replay_start.resync = host::test_env::getenv("MELEE_REPLAY_RESYNC") != nullptr;   // Legacy plays without resync too
  g_replay_start.codes = replay_codes(*s, &g_replay_start.ps_frozen_toggle);
  // Which fighter each port's inputs were for: the game refuses the replay when the character the
  // block names makes another fighter here (recorded with other installed fighters, mu_match_check.c).
  for (int i = 0; i < 4; ++i) g_replay_start.first_kind[i] = 0xFF;
  if (g_replay->DoesFrameExist(Slippi::GAME_FIRST_FRAME)) {
    const Slippi::FrameData* first = g_replay->GetFrame(Slippi::GAME_FIRST_FRAME);
    for (uint8_t port = 0; port < 4; ++port) {
      const auto it = first->players.find(port);
      if (it != first->players.end() && it->second.hasPost) g_replay_start.first_kind[port] = it->second.internalCharacterId;
    }
  }
  host::log("replay: %s, Slippi %s, frames %d..%d, stage %u, seed %08X, frozen PS %u (code toggle %u)%s",
            path, g_replay->GetVersionString().c_str(), Slippi::GAME_FIRST_FRAME, g_replay->GetLatestIndex(),
            (unsigned)s->stage, s->randomSeed, (unsigned)s->isFrozenPS, (unsigned)g_replay_start.ps_frozen_toggle,
            g_replay_start.resync ? ", resync on" : "");
  // The viewer's controls: this engine can keep states of the game and return to them.
  replay_control::Engine engine;
  engine.keep = replay_keep;
  engine.restore = replay_restore;
  {
    // The players, for the takeover menu: "P1 Falco".
    static const char* const kNames[26] = {"Captain Falcon", "Donkey Kong", "Fox", "Mr. Game & Watch", "Kirby", "Bowser",
        "Link", "Luigi", "Mario", "Marth", "Mewtwo", "Ness", "Peach", "Pikachu", "Ice Climbers", "Jigglypuff", "Samus",
        "Yoshi", "Zelda", "Sheik", "Falco", "Young Link", "Dr. Mario", "Roy", "Pichu", "Ganondorf"};
    std::string names[4];
    uint32_t ports = 0;
    for (int i = 0; i < 4; ++i) {
      if (!g_replay->DoesPlayerExist((int8_t)i)) continue;
      ports |= 1u << i;
      const uint8_t c = s->players[(uint8_t)i].characterId;
      names[i] = "P" + std::to_string(i + 1) + " " + (c < 26 ? kNames[c] : "Fighter");
    }
    replay_control::set_players(ports, names);
  }
  replay_control::set_replay_path(path);
  replay_control::begin(Slippi::GAME_FIRST_FRAME, g_replay->GetLatestIndex(), engine);
  host::set_replay_viewing(true);
  return true;
}
#endif

}  // namespace source_port
