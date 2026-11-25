// Source Port host: the console the game calls into. Log, panic, clocks, polling, retraces, the GX
// FIFO, VI, controllers and rumble, audio DMA, DSP mail, ARAM, the native allocator, sound mode, stop.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

bool trace_ax_voice_window() {
  const char* range = host::test_env::getenv("MELEE_TRACE_AX_SFX");
  unsigned first = 0, last = 0;
  const uint32_t retrace = host::retrace_count();
  return range && std::sscanf(range, "%u:%u", &first, &last) == 2 &&
         first <= last && retrace >= first && retrace <= last;
}

void trace_ax_voice(const ax::VoiceTrace& trace) {
  if (!trace_ax_voice_window()) return;
  host::log("[ax-voice-native] retrace=%u block=%llu ms=%u pb=%08X cur=%08X>%08X end=%08X ratio=%08X frac=%04X>%04X env=%04X/%04X input=%d/%d peak=%d output=%d/%d peak=%d",
            host::retrace_count(), (unsigned long long) trace.frame,
            trace.millisecond, trace.pb_addr, trace.cur_before,
            trace.cur_after, trace.end_addr, trace.ratio, trace.frac_before,
            trace.frac_after, trace.volume_before, trace.volume_delta,
            trace.input_first, trace.input_last, trace.input_peak,
            trace.output_first, trace.output_last, trace.output_peak);
}

// MELEE_TRACE_AX_VOICES (M4 diagnostic, off by default): every AX frame and every voice block the
// ucode renders, with hashes of the voice's own samples before the final mix (ax_ucode.h).
void trace_ax_frame(const char* line) {
  host::log("[ax-vtrace] retrace=%u tb=%llu vi=%llu %s", host::retrace_count(),
            (unsigned long long)(host::cpu ? host::cpu->tb : 0),
            (unsigned long long)(host::next_retrace_tb() - host::TB_PER_FRAME), line);
}

uint16_t ax_native_rd16(uint32_t addr) {
  uint16_t value;
  std::memcpy(&value, host::ptr(addr, sizeof value), sizeof value);
  return value;
}
uint32_t ax_native_rd32(uint32_t addr) {
  uint32_t value;
  std::memcpy(&value, host::ptr(addr, sizeof value), sizeof value);
  return value;
}
void ax_native_wr16(uint32_t addr, uint16_t value) {
  std::memcpy(host::ptr(addr, sizeof value), &value, sizeof value);
}
void ax_native_wr32(uint32_t addr, uint32_t value) {
  std::memcpy(host::ptr(addr, sizeof value), &value, sizeof value);
}

// Buttons held on any controller at the last pad read, for the stage pick of a disc with alternate
// stages (host::cosmetics::set_stage_button_probe).
std::atomic<uint32_t> g_held_buttons{0};
uint32_t held_buttons() { return g_held_buttons.load(std::memory_order_relaxed); }

// ---- the table the game calls ----
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#endif
void h_log(const char* text) {
  std::string line = text ? text : "";
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#endif
  if (line.rfind("online: match starts,", 0) == 0) {
    using namespace host::rollback;
    rollback_log(true);
    accumulator.start(now_ns(), presentation);
  }
  if (line.rfind("online: match exit:", 0) == 0) rollback_log(true);
  // MELEE_TRACE_AX_VOICES: the game's sound-request and stream traces also get the timebase, to
  // place each on the AX frame timeline (the game's own lines carry only the retrace).
  static const bool request_tb = host::test_env::getenv("MELEE_TRACE_AX_VOICES") != nullptr;
  if (request_tb && (line.rfind("[ax-", 0) == 0 || line.rfind("[audio-start-native]", 0) == 0)) {
    host::log("[game] %s tb=%llu", line.c_str(), (unsigned long long)(host::cpu ? host::cpu->tb : 0));
    return;
  }
  if (!line.empty()) host::log("[game] %s", line.c_str());
}
void h_panic(const char* file, int32_t line, const char* message) {
  host::log("game panic at %s:%d", file ? file : "?", line);
  void* frames[32];
  const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    const uintptr_t pc = reinterpret_cast<uintptr_t>(frames[i]);
    if (pc > GAME_IMAGE_BASE && pc < GAME_IMAGE_BASE + host::game_image_size)
      host::log("  stack: melee_game.dll+0x%llX", (unsigned long long)(pc - 1 - GAME_IMAGE_BASE));
  }
#ifdef __APPLE__
  host::log("game panic: %s", message ? message : "");
  host::end_process(1);
#endif
  host::die("game stopped at %s:%d: %s", file ? file : "?", line, message ? message : "");
}
uint64_t h_ticks() {

  if (g_resim) rollback_frame_callback();
  return host::cpu->tb;
}
// The date the game's calendar works from. It was zero, so every save and every Training Mode CE
// export was dated January 1, 2000.
uint64_t h_boot_time() {

  return host::console_epoch_ticks();
}




void native_audio_done(void*) {
  if (g_game.ai_dma_done) g_game.ai_dma_done();
}

void native_audio_tick() {
  audio_core::tick(g_audio, host::cpu->tb, host::TB_HZ, native_audio_done, nullptr);
}

void h_poll() {
  // A re-simulated frame never waits on the machine; count it if something does.
  if (g_resim) { ++g_resim_polls; return; }
  guarded([] {
    host::advance_time(2048);   // time flows in busy waits, as it does for the recompiled build
    native_audio_tick();
    if (host::retrace_due()) host::retrace();
    else g_game.fire_alarms(host::cpu->tb);
  });
}

void native_retrace() {
  rollback_log();
  native_audio_tick();
  g_game.fire_alarms(host::cpu->tb);
  g_game.retrace();
}

void h_gx_fifo(const uint8_t* data, uint32_t size) {
  if (gl_costs_enabled() && g_resim) g_resim_gx_bytes += size;
  guarded([&] { gx::write_fifo_bytes(data, size); });
}
void h_native_render_scope_event(const MuNativeRenderScopeEvent* event) {
  if (!event) return;
  gx::native_render_scope_event({event->sequence, event->scope_id, event->phase, event->reserved});
}
int32_t h_native_pose_capture_enabled() { return gx::native_pose_capture_enabled() ? 1 : 0; }
void h_native_pose_snapshot(const MuNativePoseSnapshot* snapshot) {
  if (!snapshot) return;
  guarded([&] { gx::native_pose_snapshot(snapshot); });
}
uint32_t h_music_volume() { return (uint32_t) slippi::jukebox::user_volume(); }
int32_t h_native_owner_tracking() { return gx::owner_tracking_enabled() ? 1 : 0; }
// Always on: the scope tokens are a few bytes per PObj, and without them a draw is known only by
// its display list and call order, which shifts whenever a menu drops or adds a draw.
int32_t h_native_draw_identity() { return 1; }
// Render semantics for the ray-traced renderer (tokens 0xF3-0xF6): only while it asks for them.
// DORMANT: that renderer was removed, so this answers 0 unless MELEE_RTX_SEMANTICS=1 (rtx_semantics.h).
int32_t h_native_rtx_semantics() { return gx::rtx_semantics_enabled() ? 1 : 0; }
void h_vi_configure(uint32_t w, uint32_t h, uint32_t interlaced) { host::log("VI: %ux%u%s", w, h, interlaced ? " interlaced" : ""); }
void h_vi_set_next_framebuffer(void*) {}   // presentation follows the EFB copy, as in the recompiled build
void h_vi_flush() {}
uint32_t h_vi_retrace_count() {

  return host::retrace_count();
}
uint32_t h_vi_next_field() {

  return host::retrace_count() & 1;
}
void h_vi_set_black(int32_t) {}
void h_vi_wait_retrace() {
  if (g_resim) { ++g_resim_vi_waits; return; }
  guarded([] { host::retrace(); });
}

// The idle wait before a retrace, one audio period at a time (host API 16). Plays each block at
// the real time of its AI deadline instead of three together after the retrace. Alarms and the
// retrace itself stay where they were (native_retrace), so game code runs in the same order.
// MELEE_AUDIO_PACING=0 turns it off for comparisons.
int32_t h_vi_idle_step() {
   // wall-time audio pacing cannot schedule game work
  static const bool enabled = [] { const char* v = std::getenv("MELEE_AUDIO_PACING"); return !(v && *v == '0'); }();
  // MELEE_AUDIO_PACING_TRACE=1: why the idle loop stops, counted and logged every 600 calls.
  static const bool trace = [] { const char* v = std::getenv("MELEE_AUDIO_PACING_TRACE"); return v && *v == '1'; }();
  static uint64_t calls, stop_off, stop_awaiting, stop_past_retrace, steps;
  if (trace && ++calls % 600 == 0)
    host::log("audio pacing: %llu calls, %llu steps; stops: awaiting block %llu, deadline past retrace %llu, off %llu",
              (unsigned long long)calls, (unsigned long long)steps, (unsigned long long)stop_awaiting,
              (unsigned long long)stop_past_retrace, (unsigned long long)stop_off);
  if (!enabled || g_resim || !g_audio.running || !g_audio.dma_length) { ++stop_off; return 0; }
  // The game has not handed over the next block yet: nothing to play at the next deadline.
  if (g_audio.native_pcm && g_audio.awaiting_block) { ++stop_awaiting; return 0; }
  int32_t stepped = 0;
  guarded([&] {
    const uint64_t due = g_audio.next_tb;
    if (host::cpu->tb < due && !host::wait_until_console_time(due)) { ++stop_past_retrace; return; }
    native_audio_tick();
    stepped = 1;
    ++steps;
  });
  return stepped;
}

// --online-test ranked, once a frame (the game reads its pads every frame, in every scene). A game
// that ended and is not back in Debug VS's match scene within 5 s (a results screen waiting for a
// press) is asked back into it for the set's next game; once the set's room has closed the run ends.


void h_pad_read(MuPadStatus out[4]) {
  host::PadState pads[4];
  host::input_poll(pads);
  lcancel::apply(pads, true); // Ranked brackets keep display-only feedback
  bool offline_gameplay = false;
  if (gx::RenderOptions::live_offline_delay() && !g_replaying &&
      slippi::online::session_mode() < 0 && g_game.lcancel_view) {
    MuLcancelView view{};
    g_game.lcancel_view(&view);
    for (const auto& fighter : view.port) offline_gameplay |= fighter.present != 0;
  }
  host::offline_delay::apply(pads, offline_gameplay);
  // The player's data-only Gecko codes, once per frame. Offline only: the writer in the game
  // library also refuses during an online match, whichever kind.
  if (slippi::online::session_mode() < 0 && !g_replaying) user_gecko::apply();
  else {
    user_gecko::suspend_function_patches();   // a code's rewritten functions go back to their original code online and during playback
    // Online: only the codes both players agreed to run (gecko_session.h), on both games alike.
    if (!g_replaying && slippi::online::is_online_match()) user_gecko::run_agreed(gecko_session::match_codes());
  }
  // A pasted always-display hitbox code turns on the Arcade look switches its lines stand for.
  gx::set_arcade_display(gx::ArcadeSource::Codes, user_gecko::arcade_bits_of_codes());
  // Hidden test hook: MELEE_TEST_INSTALL_LOOK=<0 Diet, 1 Animelee, 2 Animelee x DBZ> starts that one-click install once.
  {
    static const int test_install = [] { const char* v = host::test_env::getenv("MELEE_TEST_INSTALL_LOOK"); return v && *v ? std::atoi(v) : -1; }();
    static int frames = 0;
    if (test_install >= 0 && ++frames == 120) {
      wchar_t exe[MAX_PATH] = {};
      std::string tool;
      if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) tool = (std::filesystem::path(exe).parent_path() / L"tools" / L"xdelta3.exe").u8string();
      host::cosmetics::install_official_look(test_install, host::options.iso, tool, source_port::mods::mods_folder().u8string());
    }
  }
  if (host::audio_tracing()) {
    // Latency trace: the first read that shows a new press on port 1 (buttons or the stick leaving
    // its center), the moment the game sees it.
    static bool was_active = false;
    const bool active = pads[0].err == 0 && ((pads[0].button & 0x0F7Fu) || std::abs((int)pads[0].stick_y) >= 40 ||
                                             std::abs((int)pads[0].stick_x) >= 40);
    if (active && !was_active) host::audio_trace_event("input");
    was_active = active;
  }
  g_held_buttons.store((uint32_t)(pads[0].button | pads[1].button | pads[2].button | pads[3].button), std::memory_order_relaxed);
  for (int i = 0; i < 4; ++i) {
    out[i].button = pads[i].button;
    out[i].stick_x = pads[i].stick_x; out[i].stick_y = pads[i].stick_y;
    out[i].sub_x = pads[i].sub_x; out[i].sub_y = pads[i].sub_y;
    out[i].trigger_l = pads[i].trig_l; out[i].trigger_r = pads[i].trig_r;
    out[i].analog_a = pads[i].analog_a; out[i].analog_b = pads[i].analog_b;
    out[i].err = pads[i].err;
  }
  // 20XX TE input display (offline matches and replays): the controllers the game read.
  training_overlay::set_input_display(te_host_feature(MU_GAME_OPTION2_TE_INPUT_DISPLAY));
  // Training Lab overlay: what the game read this frame, with the fighters' state.
  if (((gx::RenderOptions::live_te_options() & (MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_OVERLAY)) ==
           (MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_OVERLAY) || training_overlay::input_display()) &&
      g_game.state_snapshot) {
    MuStatePod state{};
    g_game.state_snapshot(&state);
    training_overlay::publish(state, out);
  }
  if (!host::options.input_log.empty()) {   // --input-log, same CSV as the recompiled build
    static FILE* input_log = std::fopen(host::options.input_log.c_str(), "w");
    if (input_log) {
      static bool header = (std::fputs("retrace,port,buttons,stick_x,stick_y,cstick_x,cstick_y,trigger_l,trigger_r\n", input_log), true);
      (void)header;
      for (int i = 0; i < 4; ++i)
        if (pads[i].err == 0)
          std::fprintf(input_log, "%u,%d,%04X,%d,%d,%d,%d,%u,%u\n", host::retrace_count(), i + 1, pads[i].button, pads[i].stick_x, pads[i].stick_y,
                       pads[i].sub_x, pads[i].sub_y, pads[i].trig_l, pads[i].trig_r);
      std::fflush(input_log);
    }
  }
}
// The game's motor commands go through the same routing as the Static Recomp's: the Controller
// rumble setting, the device actually feeding that port, and online only the local player's slot.
// Sending them straight to the adapter ignored the setting. A replay rumbles nobody.
void h_pad_rumble(int32_t port, int32_t on) {
  if (g_replaying) on = 0;
  // Online the game already keeps only the local player's motor (mu_online_motor_port), and what
  // it passes is the controller port that player's input comes from, not their slot in the match.
  // Comparing that with the slot dropped every rumble whenever the two differ (a player who is
  // not player 1 online: no rumble at all on the Source Port, while the Static Recomp rumbled).
  if (slippi::online::is_online_match()) {
    host::input_rumble_local(on != 0);
    return;
  }
  host::input_rumble(port, on != 0);
}

void h_ai_init_dma(void* buffer, uint32_t length) {
  audio_core::init_dma(g_audio, buffer, length, true);
}
void h_ai_start_dma(int32_t on) {
  const bool was_running = g_audio.running;
  audio_core::start_dma(g_audio, on != 0, host::cpu->tb, host::TB_HZ);
  // Diagnostic only (M4): move the AX clock against the retrace grid by this many timebase ticks.
  static const char* shift = host::test_env::getenv("MELEE_TEST_AX_PHASE_SHIFT");
  if (!was_running && g_audio.running && shift)
    g_audio.next_tb += std::strtoull(shift, nullptr, 0);
  // MELEE_AUDIO_PHASE_US=N (measurement, 0 to 5000): start the 5 ms AX clock N microseconds later
  // against the retrace, so the first sound frame after the game's frame comes sooner after it.
  static const long phase_us = [] { const char* v = std::getenv("MELEE_AUDIO_PHASE_US"); const long n = v ? std::atol(v) : 0; return n > 0 && n < 5000 ? n : 0L; }();
  if (!was_running && g_audio.running && phase_us)
    g_audio.next_tb += (uint64_t)phase_us * host::TB_HZ / 1000000u;
  if (!was_running && g_audio.running)
    host::log("audio: AI clock phase retrace=%u next_tick_after_boundary=%lld", host::retrace_count(),
              (long long)(g_audio.next_tb - (host::next_retrace_tb() - host::TB_PER_FRAME)));
}
void h_ai_set_sample_rate(uint32_t) {}
void h_ai_set_stream_volume(int32_t, int32_t) {}
void h_dsp_mail(uint32_t mail) { audio_core::dsp_mail(mail); }
uint32_t h_dsp_mail_pending() { return audio_core::dsp_mail_pending(); }
void* h_aram_base() { return host::aram; }
uint32_t h_aram_size() { return MU_HOST_ARAM_BYTES; }
void h_aram_dma(int32_t to_aram, void* mainmem, uint32_t aram_offset, uint32_t length) {
  if ((uint64_t)aram_offset + length > MU_HOST_ARAM_BYTES) host::die("ARAM DMA out of range %08X+%X", aram_offset, length);
  if (to_aram) std::memcpy(host::aram + aram_offset, mainmem, length);
  else std::memcpy(mainmem, host::aram + aram_offset, length);
}
void* h_native_alloc(uint32_t size) {
  auto* ptr = std::malloc(size);
  return ptr;
}
void h_native_free(void* ptr) {
  std::free(ptr);
}

uint32_t h_mem1_size() { return MEM1_SIZE; }
host::ConsoleSoundMode& console_sound_mode() {
  static host::ConsoleSoundMode mode(std::filesystem::u8path(host::options.card_dir));
  return mode;
}
int32_t h_sound_mode() { return console_sound_mode().get(); }
void h_set_sound_mode(int32_t mode) {
  if (!console_sound_mode().set(mode)) host::log("audio: could not save the console sound mode");
}
int32_t h_progressive_mode() { return 0; }
void h_set_progressive_mode(int32_t) {}
int32_t h_reset_code() { return 0; }
int32_t h_reset_switch() { return 0; }
void h_stop(int32_t reason, int32_t code) {
  host::log("game stopped: reason %d code %d", reason, code);
  // A replay the game refused to start ends the run with its own code (host.h kReplayRefusedExit), and
  // its match never began, so no recording of it is written to the replay folder.
  const bool refused = reason == MU_STOP_EXIT && code == MU_STOP_REPLAY_REFUSED;
  if (refused) drop_replay_recording();
  host::request_exit(refused ? host::kReplayRefusedExit : 0);
  guarded([] { host::retrace(); });
}

}  // namespace host_internal
}  // namespace source_port
