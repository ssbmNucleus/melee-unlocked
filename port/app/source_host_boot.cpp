// Source Port host: boot and shutdown. The game library loaded (an AppContainer maps it itself), the
// table the game calls built (make_host), memory reserved, run(), crash handlers, the end of a run
// (rollback and pose statistics, the tournament finished), --match.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- stopping ----
// host::retrace() ends the game by throwing ExitRequested. That must not unwind through the game's
// frames (another compiler's code), so it is caught at the boundary and the process finishes here.
void log_native_pose_bridge_stats() {
  const auto stats = gx::native_pose_bridge_stats();
  host::log("native pose bridge: %llu snapshots, %llu rigid captured, %llu envelope scopes, %llu envelope captured, %llu invalid; captured default-setup=%llu custom-setup=%llu; rejected kind=%llu joint=%llu depth=%llu flags=%llu constraints=%llu tracks=%llu bytes=%llu format=%llu envelope-data=%llu envelope-limit=%llu bound-animation=%llu",
            (unsigned long long)stats.snapshots,
            (unsigned long long)stats.rigid_captured,
            (unsigned long long)stats.envelope_scopes,
            (unsigned long long)stats.envelope_captured,
            (unsigned long long)stats.invalid_payloads,
            (unsigned long long)stats.captured_default_setup,
            (unsigned long long)stats.captured_custom_setup,
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_KIND],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_NO_JOINT],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_CHAIN_LIMIT],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_JOINT_FLAGS],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_CONSTRAINTS],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_TRACK_LIMIT],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_TRACK_BYTES],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_TRACK_FORMAT],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_ENVELOPE_DATA],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_ENVELOPE_LIMIT],
            (unsigned long long)stats.rejected[MU_NATIVE_POSE_REJECT_BOUND_ANIMATION]);
}
// The body counter is already exposed by the read-only page-pause query (a[0]=-1).
// Unlike PADRead / VI waits, the clock callback also runs in every resim body
// (HSD_PerfSetStartTime). Only query it while catch-up is active.
uint32_t rollback_engine_frame() {
  int32_t state[3] = {-1, 0, 0};
  return g_game.practice && g_game.practice(MU_PRACTICE_PAGE_PAUSE, state, 3) == 0 && state[2] >= 0
      ? static_cast<uint32_t>(state[2]) : 0;
}
void rollback_log(bool exiting) {
  using namespace host::rollback;
  auto& a = accumulator;
  if (!a.online) return;
  const auto ns = now_ns();
  if (exiting) a.end(ns, rollback_engine_frame(), presentation);
  if (!exiting && !a.due(ns)) return;
  const auto counts = presentation.snapshot();
  auto text = line(exiting ? "match" : "interval", ns - (exiting ? a.started_ns : a.window_ns),
                   exiting ? a.match : a.interval, counts - (exiting ? a.match_present : a.interval_present));
  host::log("%s", text.c_str());
  if (exiting) a.stop(presentation);
  else a.next_window(ns, counts);
}
void rollback_frame_callback() {
  using namespace host::rollback;
  if (!accumulator.catching_up) return;
  const auto frame = rollback_engine_frame();
  if (frame > accumulator.last_frame()) {
    accumulator.frame(now_ns(), frame);
    rollback_log();
  }
}
// Both normal returns and a guarded ExitRequested must finish before shutdown closes the SDK.
void finish_tournament_before_exit() {
  return;
  if (g_game.practice) g_game.practice(MU_TOURNEY_QUIT_OP, nullptr, 0);

}

// --match sets this once, before the game starts. An ordinary run never touches it, and the game
// only asks for it where a VS match's rules are finalised.
MuMatchOverride g_match{};
bool g_match_set = false;
const MuMatchOverride* h_match_override() {

  return g_match_set ? &g_match : nullptr;
}

// --rng-seed: forced from host::options once at startup (see main.cpp's flag parsing), read by the
// game at the same first GS_VS frame callback, so a scripted run reaches identical RNG in both the source port and a
// --rng-seed recomp run from the first in-match frame.
void h_rng_seed_override(uint32_t* seed, int32_t* has_value) {

  *has_value = host::options.rng_seed_set ? 1 : 0;
  if (host::options.rng_seed_set) *seed = host::options.rng_seed;
}
// Called from the same first GS_VS frame callback unconditionally (not just when --rng-seed was given), so
// an @match-relative script section starts counting from the match's rules/players being
// finalised on an ordinary offline run too, the same instant the recomp guest's --rng-seed hook
// marks it.
void h_mark_match_start() {
  host::input_mark_match_start();
}



MuHostApi make_host() {
  MuHostApi h{};
  h.version = MU_HOST_API_VERSION;
  h.test_getenv = [](const char* name) -> const char* {
    return host::test_env::getenv(name);
  };
  h.log = h_log; h.panic = h_panic; h.ticks = h_ticks; h.boot_time = h_boot_time; h.poll = h_poll;
  h.gx_fifo = h_gx_fifo; h.vi_configure = h_vi_configure; h.vi_set_next_framebuffer = h_vi_set_next_framebuffer;
  h.vi_flush = h_vi_flush; h.vi_retrace_count = h_vi_retrace_count; h.vi_next_field = h_vi_next_field;
  h.vi_set_black = h_vi_set_black; h.vi_wait_retrace = h_vi_wait_retrace;
  h.pad_read = h_pad_read; h.pad_rumble = h_pad_rumble;
  h.disc_entrynum = h_disc_entrynum; h.disc_file = h_disc_file; h.disc_read = h_disc_read;
  h.disc_status = h_disc_status; h.disc_id = h_disc_id;
  h.card_probe = h_card_probe; h.card_mount = h_card_mount; h.card_unmount = h_card_unmount; h.card_open = h_card_open;
  h.card_close = h_card_close; h.card_create = h_card_create; h.card_delete = h_card_delete; h.card_rename = h_card_rename;
  h.card_read = h_card_read; h.card_write = h_card_write; h.card_stat = h_card_stat; h.card_set_stat = h_card_set_stat;
  h.card_free_blocks = h_card_free_blocks; h.card_format = h_card_format;
  h.ai_init_dma = h_ai_init_dma; h.ai_start_dma = h_ai_start_dma; h.ai_set_sample_rate = h_ai_set_sample_rate;
  h.ai_set_stream_volume = h_ai_set_stream_volume; h.dsp_mail = h_dsp_mail; h.dsp_mail_pending = h_dsp_mail_pending;
  h.aram_base = h_aram_base; h.aram_size = h_aram_size; h.aram_dma = h_aram_dma;
  h.native_alloc = h_native_alloc; h.native_free = h_native_free;
  h.mem1_size = h_mem1_size; h.sound_mode = h_sound_mode; h.set_sound_mode = h_set_sound_mode;
  h.progressive_mode = h_progressive_mode; h.set_progressive_mode = h_set_progressive_mode;
  h.reset_code = h_reset_code; h.reset_switch = h_reset_switch; h.stop = h_stop;
  h.match_override = h_match_override;
  h.rng_seed_override = h_rng_seed_override; h.mark_match_start = h_mark_match_start;
  h.native_pose_capture_enabled = h_native_pose_capture_enabled;
  h.native_pose_snapshot = h_native_pose_snapshot;
  h.game_options = h_game_options;
  h.native_owner_tracking = h_native_owner_tracking;
  h.music_volume = h_music_volume;
  h.replay_start = h_replay_start; h.replay_frame = h_replay_frame;
  h.replay_stock_steal = h_replay_stock_steal; h.replay_event = h_replay_event;
  h.replay_finished = h_replay_finished;
  const char* draw_audit = host::test_env::getenv("MELEE_NATIVE_DRAW_AUDIT");
  const bool audit_enabled = draw_audit && std::strcmp(draw_audit, "1") == 0;
  gx::set_native_draw_audit(audit_enabled);
  if (audit_enabled) h.native_render_scope_event = h_native_render_scope_event;
  h.native_draw_identity = h_native_draw_identity;
  h.mod_flags = h_mod_flags;
  h.slippi_command = h_slippi_command;
  h.resim_phase = h_resim_phase;
  h.online_test_match = h_online_test_match;
  h.game_options2 = h_game_options2;
  h.hud_scales = h_hud_scales;
  h.hud_player = h_hud_player;
  h.vi_idle_step = h_vi_idle_step;
  h.game_options3 = h_game_options3;
  h.display_options = h_display_options;
  h.audio_options = h_audio_options;
  h.picture_widen = h_picture_widen;
  h.hud_combo = h_hud_combo;
  h.shield_color = h_shield_color;
  h.grid_color = h_grid_color;
  h.screen_shake_scale = h_screen_shake_scale;
  h.hud_extras = h_hud_extras;
  h.native_rtx_semantics = h_native_rtx_semantics;
  h.hud_pro_tag = nullptr;   // retired with the pro AI: the slot stays for the table's layout
  return h;
}

// A crash inside the game: the faulting address and the game's return addresses on the stack, as
// offsets into melee_game.dll (resolve with addr2line -e melee_game.dbg). The application's own
// crash report still runs afterwards.
#ifndef _WIN32
// Linux: a fault inside the game logs its offset in melee_game.so (addr2line -e melee_game.dbg) and
// keeps a --replay run's recording, then hands the signal on to the application's crash report.
struct sigaction g_previous_crash_action[32];
void on_game_signal(int sig, siginfo_t* si, void* uc_void) {
  const ucontext_t* uc = static_cast<const ucontext_t*>(uc_void);
  const uint64_t base = GAME_IMAGE_BASE, end = base + host::game_image_size;
#ifdef __APPLE__
  const uint64_t rip = uc ? mac_or_linux_rip(uc) : 0;
#else
  const uint64_t rip = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RIP] : 0;
#endif
  if (rip >= base && rip < end) {
    host::mark_dying();
    write_replay_recording();
    host::log("game crash signal %d at melee_game.so+0x%llX, address %p", sig, (unsigned long long)(rip - base), si ? si->si_addr : nullptr);
#ifndef __APPLE__
    host::log("  rax %016llX rbx %016llX rcx %016llX rdx %016llX", (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
              (unsigned long long)uc->uc_mcontext.gregs[REG_RBX], (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
              (unsigned long long)uc->uc_mcontext.gregs[REG_RDX]);
    // The game's frames on its stack (GCC keeps frame pointers out, so this is the return addresses
    // that point into the game, as the Windows leaf fallback reads them).
    const uint64_t* sp = (const uint64_t*)uc->uc_mcontext.gregs[REG_RSP];
#else
    const uint64_t* sp = (const uint64_t*)uc->uc_mcontext->__ss.__rsp;
#endif
    for (int i = 0, shown = 0; i < 512 && shown < 24; ++i) {
      const uint64_t v = sp[i];
      if (v > base && v < end) { host::log("  stack: melee_game.so+0x%llX", (unsigned long long)(v - 1 - base)); ++shown; }
    }
  }
  struct sigaction& previous = g_previous_crash_action[sig & 31];
  if (previous.sa_flags & SA_SIGINFO) { if (previous.sa_sigaction) previous.sa_sigaction(sig, si, uc_void); }
  else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler) previous.sa_handler(sig);
  else { signal(sig, SIG_DFL); raise(sig); }
}
void install_game_signal_handler() {
  struct sigaction sa{};
  sa.sa_sigaction = on_game_signal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, &g_previous_crash_action[sig & 31]);
}
#else
LONG CALLBACK on_game_exception(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code < 0x80000000u || code == 0xE06D7363u /* C++ exception */) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t base = GAME_IMAGE_BASE, end = base + host::game_image_size;
  const uint64_t rip = info->ContextRecord->Rip;
  const bool in_game = rip >= base && rip < end;
  if (!in_game) {
    // A bad indirect call has already left the DLL. Its return address still
    // identifies the game caller, and the unwind below starts from that frame.
    bool called_by_game = false;
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2 &&
        info->ExceptionRecord->ExceptionInformation[0] == 8) {
      uint64_t caller = 0;
      SIZE_T read = 0;
      if (ReadProcessMemory(GetCurrentProcess(), (const void*)info->ContextRecord->Rsp,
                            &caller, sizeof caller, &read) && read == sizeof caller) {
        called_by_game = caller >= base && caller < end;
      }
    }
    if (!called_by_game) return EXCEPTION_CONTINUE_SEARCH;
  }
  host::mark_dying();
  write_replay_recording();   // a --replay run keeps what it played up to the crash
  if (in_game)
    host::log("game crash %08lX at melee_game.dll+0x%llX", code, (unsigned long long)(rip - base));
  else
    host::log("game crash %08lX calling %016llX", code, (unsigned long long)rip);
  const CONTEXT& r = *info->ContextRecord;
  if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
    host::log("  %s address %016llX", info->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" :
              (info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading"),
              (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
  host::log("  rax %016llX rbx %016llX rcx %016llX rdx %016llX", r.Rax, r.Rbx, r.Rcx, r.Rdx);
  host::log("  rsi %016llX rdi %016llX r8  %016llX r9  %016llX", r.Rsi, r.Rdi, r.R8, r.R9);
  // The real call chain, from the unwind tables GCC writes into the DLL (.pdata), so frames are
  // callers rather than whatever return-address-looking values happen to sit on the stack.
  CONTEXT ctx = r;
  for (int depth = 0; depth < 24; ++depth) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
#ifdef _MSC_VER
    __try {
      if (fn) {
        void* handler_data = nullptr; DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
      } else {
        ctx.Rip = *(const DWORD64*)ctx.Rsp;   // a leaf: the return address is on top
        ctx.Rsp += 8;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
#else
    // MinGW cannot protect RtlVirtualUnwind with MSVC SEH. Read a leaf's return address
    // through the checked self-process API; stop at a non-leaf rather than risking a second fault.
    if (fn) break;
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void*)ctx.Rsp, &ctx.Rip, sizeof ctx.Rip, &read) ||
        read != sizeof ctx.Rip) break;
    ctx.Rsp += 8;
#endif
    if (ctx.Rip == 0) break;
    if (ctx.Rip >= base && ctx.Rip < end) host::log("  stack: melee_game.dll+0x%llX", (unsigned long long)(ctx.Rip - 1 - base));
    else { host::log("  (leaves the game at %016llX)", (unsigned long long)ctx.Rip); break; }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace host_internal

bool set_match(const char* spec) {
  if (!spec || !*spec) return false;
  MuMatchOverride m{};
  for (auto& p : m.players) p.kind = -1;

  const char* p = spec;
  char* end = nullptr;
  const long stage = std::strtol(p, &end, 0);
  if (end == p || stage < 0) return false;
  m.stage = (int32_t)stage;
  p = end;

  size_t slot = 0;
  while (*p == ':') {
    ++p;
    if (slot >= sizeof m.players / sizeof m.players[0]) return false;
    const long kind = std::strtol(p, &end, 0);
    if (end == p || kind < 0) return false;
    m.players[slot].kind = (int32_t)kind;
    p = end;
    // Each player's options, in any order: /c<level> makes it a CPU, /x<costume> picks a costume.
    while (*p == '/') {
      const char opt = p[1];
      if (opt != 'c' && opt != 'x') return false;
      p += 2;
      const long v = std::strtol(p, &end, 0);
      if (end == p || v < 0) return false;
      if (opt == 'c') { m.players[slot].cpu = 1; m.players[slot].cpu_level = (int32_t)v; }
      else { m.players[slot].costume = (int32_t)v; }
      p = end;
    }
    ++slot;
  }
  if (*p != '\0' || slot == 0) return false;   // trailing junk, or no players at all

  g_match = m;
  g_match_set = true;
  return true;
}


#ifdef _WIN32
// Inside an AppContainer Windows relocates every image it loads,
// even one built for a fixed base with ASLR off, and the game image works only at GAME_IMAGE_BASE (its
// 32-bit pointers sit beside MEM1). There the host maps the image itself, the way the loader would:
// sections at their addresses, imports bound, page protections, the exception tables registered, the
// TLS callbacks and the entry point run. The image uses no implicit TLS (its .tls holds only the
// runtime's callbacks), so there is no TLS slot to set up. Everywhere else LoadLibrary does it.
static bool in_app_container() {
  HANDLE token = nullptr;
  DWORD is = 0, len = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  const bool ok = GetTokenInformation(token, TokenIsAppContainer, &is, sizeof is, &len);
  CloseHandle(token);
  return ok && is != 0;
}

// Found as LoadLibrary would: a bare name sits beside the program (the working directory is the
// sandbox's data folder).
static std::filesystem::path game_library_file(const std::string& path) {
  std::filesystem::path file_path = std::filesystem::u8path(path);
  if (file_path.is_relative()) {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) file_path = std::filesystem::path(exe).parent_path() / file_path;
  }
  return file_path;
}
static HMODULE map_game_image(const std::string& path, std::string* error) {
  const std::filesystem::path file_path = game_library_file(path);
  std::ifstream in(file_path, std::ios::binary);
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto* dos = (const IMAGE_DOS_HEADER*)file.data();
  if (file.size() < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > file.size()) {
    *error = "not a Windows image";
    return nullptr;
  }
  const auto* nt = (const IMAGE_NT_HEADERS*)(file.data() + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.ImageBase != GAME_IMAGE_BASE) {
    *error = "not the x64 game image for this base";
    return nullptr;
  }
  const size_t size = nt->OptionalHeader.SizeOfImage;
  // Committed inside the range held since startup (g_game_image_reservation), so nothing can take it in
  // between. GAME_IMAGE_HOLD_END bounds the image to what GX can address.
  if (!g_game_image_reservation.base || size > GAME_IMAGE_HOLD_END - GAME_IMAGE_BASE) { *error = "its address range is not held"; return nullptr; }
  auto* base = (uint8_t*)VirtualAlloc((void*)GAME_IMAGE_BASE, size, MEM_COMMIT, PAGE_READWRITE);
  if (base != (uint8_t*)GAME_IMAGE_BASE) {
    *error = "its address range could not be committed";
    return nullptr;
  }
  std::memcpy(base, file.data(), std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, file.size()));
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const size_t raw = std::min<size_t>(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
    if (sec[i].VirtualAddress + raw > size || sec[i].PointerToRawData + raw > file.size()) { *error = "a section is out of bounds"; VirtualFree(base, size, MEM_DECOMMIT); return nullptr; }
    std::memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, raw);
  }
  // Imports: the system's runtime and kernel32, bound by name or ordinal.
  const auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress); imp.Size && d->Name; ++d) {
    const char* dll = (const char*)(base + d->Name);
    HMODULE lib = LoadLibraryA(dll);
    if (!lib) { *error = std::string("cannot load ") + dll; VirtualFree(base, size, MEM_DECOMMIT); return nullptr; }
    auto* names = (IMAGE_THUNK_DATA64*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    auto* slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      FARPROC fn = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)
                       ? GetProcAddress(lib, (LPCSTR)IMAGE_ORDINAL64(names->u1.Ordinal))
                       : GetProcAddress(lib, ((const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name);
      if (!fn) { *error = std::string("missing import from ") + dll; VirtualFree(base, size, MEM_DECOMMIT); return nullptr; }
      slots->u1.Function = (ULONGLONG)fn;
    }
  }
  // Page protections from the section flags, the headers read-only.
  DWORD old = 0;
  VirtualProtect(base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const DWORD c = sec[i].Characteristics;
    const size_t len = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
    if (!len) continue;
    const bool x = c & IMAGE_SCN_MEM_EXECUTE, w = c & IMAGE_SCN_MEM_WRITE;
    VirtualProtect(base + sec[i].VirtualAddress, len, x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY), &old);
  }
  FlushInstructionCache(GetCurrentProcess(), base, size);
  // Unwinding (C++ exceptions, the crash handler's stack walk) needs the image's function table.
  const auto& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (pdata.Size) RtlAddFunctionTable((PRUNTIME_FUNCTION)(base + pdata.VirtualAddress), pdata.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)base);
  using Init = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
  const auto& tls = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if (tls.Size) {
    const auto* dir = (const IMAGE_TLS_DIRECTORY64*)(base + tls.VirtualAddress);
    for (auto* cb = (PIMAGE_TLS_CALLBACK*)dir->AddressOfCallBacks; cb && *cb; ++cb) (*cb)((PVOID)base, DLL_PROCESS_ATTACH, nullptr);
  }
  if (nt->OptionalHeader.AddressOfEntryPoint && !((Init)(base + nt->OptionalHeader.AddressOfEntryPoint))((HINSTANCE)base, DLL_PROCESS_ATTACH, nullptr)) {
    *error = "its initialisation failed";
    return nullptr;
  }
  return (HMODULE)base;
}

// GetProcAddress for an image the loader never saw (map_game_image).
static FARPROC image_export(HMODULE module, const char* name) {
  auto* base = (const uint8_t*)module;
  const auto* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
  const auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!ed.Size) return nullptr;
  const auto* dir = (const IMAGE_EXPORT_DIRECTORY*)(base + ed.VirtualAddress);
  const auto* names = (const DWORD*)(base + dir->AddressOfNames);
  const auto* ordinals = (const WORD*)(base + dir->AddressOfNameOrdinals);
  const auto* functions = (const DWORD*)(base + dir->AddressOfFunctions);
  for (DWORD i = 0; i < dir->NumberOfNames; ++i)
    if (std::strcmp((const char*)(base + names[i]), name) == 0) return (FARPROC)(base + functions[ordinals[i]]);
  return nullptr;
}

#endif  // _WIN32: AppContainer and PE image loading

bool reserve_memory() {
  // First, before anything else in the process can take the range: the game's 32-bit disc pointers
  // expect its memory where the console had it.
#ifdef __APPLE__
  std::string error;
  void* mem1 = mac_elf::activate_mem1(&error) ? (void*)MEM1_BASE : nullptr;
  if (!mem1) host::log("Mac memory: %s", error.c_str());
#else
  void* mem1 = VirtualAlloc((void*)MEM1_BASE, MEM1_SIZE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
#endif
  void* lc = VirtualAlloc((void*)LOCKED_CACHE_BASE, LOCKED_CACHE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (mem1 != (void*)MEM1_BASE || lc != (void*)LOCKED_CACHE_BASE) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "Could not reserve the game's memory at 0x80000000 (%p) or 0xE0000000 (%p).", mem1, lc);
    host::log("%s", msg);
    std::fprintf(stderr, "%s\n", msg);
    if (!host::g_no_dialogs) MessageBoxA(nullptr, msg, product::kName, MB_ICONERROR | MB_OK);
    return false;
  }
  host::ram = (uint8_t*)MEM1_BASE;
  host::ram_size = 64u << 20;   // MEM1 and the game image after it: everything GX can address
  return true;
}

int run(void (*shutdown)(int)) {
  g_shutdown = shutdown;
  gx::set_native_pose_capture_enabled(gx::authored_capture_enabled());
  host::init_state_digest();
  if (!host::aram) host::aram = (uint8_t*)std::calloc(MU_HOST_ARAM_BYTES, 1);
  if (!host::cpu) host::cpu = new ppc::Context();   // only its timebase is used: the clock both builds share
  ax::set_memory({ax_native_rd16, ax_native_rd32, ax_native_wr16, ax_native_wr32,
                  host::aram, MU_HOST_ARAM_BYTES});
  ax::set_voice_trace(host::test_env::getenv("MELEE_TRACE_AX_SFX") ? trace_ax_voice
                                                        : nullptr);
  ax::set_frame_trace(host::test_env::getenv("MELEE_TRACE_AX_VOICES") ? trace_ax_frame : nullptr);
  audio_core::reset(g_audio);
  slippi::jukebox::set_disc_reader(jukebox_disc_read);
  mods::status().source_port = true;
  // Tests only: 20XX TE's features without mounting its save (its menu memory changes scripted runs).
  if (host::test_env::getenv("MELEE_TEST_TE_OWNED"))
    mods::status().te_owned = true;
  if (!read_fst()) host::die("this disc image has no readable file table. Use a clean, uncompressed Melee NTSC 1.02 ISO (a trimmed or compressed image will not work)");
  // A readable FST and retail DOL do not prove that the disc's assets exist. A damaged image can
  // have zero-filled file extents, which otherwise reach lbArchive_InitializeDAT during boot.
  // Check the first required archive on the disc before building menus or entering the game DLL.
  {
    const auto rumble = g_paths.find("/lbrb.dat");
    if (rumble == g_paths.end() || g_fst[rumble->second].dir)
      host::die("The game ISO is missing LbRb.dat. Select a fresh, uncompressed Melee NTSC 1.02 ISO.");
    const auto& file = g_fst[rumble->second];
    uint8_t header[0x20]{};
    std::string error;
    if (!host::disc_read(file.offset, header, sizeof header))
      host::die("The game ISO cannot read LbRb.dat. Select a fresh, uncompressed Melee NTSC 1.02 ISO.");
    if (!host::disc_archive_header(header, sizeof header, file.length, &error))
      host::die("The game ISO contains damaged LbRb.dat: %s. Select a fresh, uncompressed Melee NTSC 1.02 ISO; rebuild a modified ISO from a working base image.", error.c_str());
  }
  // A game library built with the native Akaneia fighters exports this name; one built without them
  // does not, and an Akaneia disc stays refused. Looked up without running any of its code.
  {
    const char* named = host::test_env::getenv("MELEE_GAME_DLL");   // nothing in a public build
#ifndef _WIN32
    // The library cannot be loaded twice at its fixed address, so its exported names are read from
    // the file: the symbol name sits in the dynamic string table only when it is exported.
    {
      std::filesystem::path library = named ? named : g_dll;
#ifndef __APPLE__
      // Read the file the loader will map: LoadLibraryA (port/linux/compat) takes a bare name from
      // beside the program first, while the app's Linux jail may start the game in its data folder.
      if (library.u8string().find('/') == std::string::npos) {
        std::error_code ec;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && std::filesystem::exists(self.parent_path() / library, ec)) library = self.parent_path() / library;
      }
#endif
      std::ifstream file(library, std::ios::binary);
      if (!file) host::log("mods: cannot read %s to check for native Akaneia fighters", library.u8string().c_str());
      const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      static const char kName[] = "mu_ak_native_build";
      if (bytes.find(std::string(kName, sizeof kName)) != std::string::npos) {
        allow_native_akaneia(true);
        host::log("mods: game library with native Akaneia fighters; Akaneia discs are accepted");
      }
    }
#else
    const std::string probed = named ? std::filesystem::absolute(named).string() : g_dll;
    if (HMODULE probe = LoadLibraryExA(probed.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)) {
      const bool native_akaneia = GetProcAddress(probe, "mu_ak_native_build") != nullptr;
      FreeLibrary(probe);
      if (native_akaneia) {
        allow_native_akaneia(true);
        host::log("mods: game library with native Akaneia fighters; Akaneia discs are accepted");
      }
    }
#endif
  }
  load_mod_overlay();
  index_music_paths();
  slippi::jukebox::set_music_pack_reader(jukebox_music_pack_read);
  check_replay_content();
  if (!g_replay && !host::gameplay::vanilla_only() && !host::options.vanilla_game) {
    // Installed costumes are costume slots of their own on the character select (host/costume_slots.h):
    // a saved choice that put one over its retail colour on every port is the standard costume again.
    host::cosmetics::set_imports_as_costume_slots(true);
    std::string error;
    const uint32_t released = host::cosmetics::release_imported_selections(&error);
    if (released || !error.empty())
      host::log("costume slots: %u saved choices of installed costumes released%s%s", released, error.empty() ? "" : ": ",
                error.c_str());
  }
  load_cosmetics(g_replay || g_online_test_mode >= 0);
  {
    std::vector<gx::texpack::CosmeticCompanion> companions;
    for (const auto& item : host::cosmetics::active_companions())
      companions.push_back({item.kind, item.target_path, item.path});
    gx::texpack::set_cosmetic_companions(std::move(companions));
  }
  // rogueMelee is played offline on its own disc, where Slippi Dolphin applies none of Slippi's codes
  // (its disc id is not GALE01): its menu files carry the Rogue Mode label, and Slippi's would hide it.
  load_system_files(g_replay ? "--replay"
                    : g_online_test_mode >= 0 ? "--online-test"
                    : host::options.vanilla_game ? "--vanilla-game"
                    : mods::status().rogue ? "rogueMelee disc, played offline with its own menus" : nullptr);
  publish_menu_mods(g_replay || g_online_test_mode >= 0);   // the menu mods installed (none in a replay)
  if (g_resume_online)
    host::log(g_slippi_menus ? "resume: online (existing boot-to-Online menu; no search)"
                            : "resume: online ignored (Online boot menu unavailable)");
  apply_content_mode(-1);   // offline: the mod's files, if a profile is loaded
#ifndef MELEE_NO_SLIPPI
  if (g_resume_online) slippi::online::request_boot_online();
#endif
  // The native game reads the retail DOL's data and the retail file formats; a modded disc (patched
  // DOL, m-ex) is refused here with a message instead of failing somewhere inside the game.
  if (!host::disc_has_vanilla_dol()) {

    host::die("ISO DOL does not match vanilla Melee NTSC 1.02; the Source engine runs only the retail game (modded discs are not supported)");
  }
  // Diagnostic builds of the game (for example the M0 maths audit) without replacing the shipped DLL.
  // A full path: a DLL is never looked for in the current folder (main.cpp, SetDefaultDllDirectories).
  // A test hook (host::test_env): a public build always loads the library beside the program.
  if (const char* dll = host::test_env::getenv("MELEE_GAME_DLL")) g_dll = std::filesystem::absolute(dll).string();
#ifndef MELEE_NO_SLIPPI
  // With the spectator server on, offline matches are recorded too, for the broadcast only (no .slp
  // unless --record-native asked for one): Slippi Dolphin sends every game it records, offline ones
  // included. Set before the game library loads, so its C runtime sees it.
  if (!std::getenv("MELEE_SOURCE_RECORD") &&
      gx::RenderOptions::live_slippi_spectator()) {
    _putenv_s("MELEE_SOURCE_RECORD", "2");
    SetEnvironmentVariableA("MELEE_SOURCE_RECORD", "2");
  }
#endif
#ifdef __APPLE__
  std::string mac_error;
  if (!g_mac_image.load(g_dll, &mac_error)) host::die("%s", mac_error.c_str());
  HMODULE module = (HMODULE)GAME_IMAGE_BASE;
  host::game_image = (uint8_t*)GAME_IMAGE_BASE;
  host::game_image_size = g_mac_image.size();
  auto game_proc = [&](const char* name) { return g_mac_image.symbol(name); };
#else
#ifndef _WIN32
  g_game_image_reservation.release();
  HMODULE module = LoadLibraryA(g_dll.c_str());
  if (!module) host::die("cannot load %s (%s)", g_dll.c_str(), dlerror());
  // The ELF's load segments: the game must start at GAME_IMAGE_BASE, its 32-bit disc pointers and
  // GX's physical window depend on it (sourceport/cmake/game-linux-layout.ld places it there).
  {
    struct Scan { uint64_t lo = ~0ull, hi = 0; } scan;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* user) {
      auto& sc = *static_cast<Scan*>(user);
      bool ours = false;
      for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_LOAD && info->dlpi_addr + info->dlpi_phdr[i].p_vaddr == GAME_IMAGE_BASE) ours = true;
      if (!ours) return 0;
      for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) continue;
        sc.lo = std::min<uint64_t>(sc.lo, info->dlpi_addr + p.p_vaddr);
        sc.hi = std::max<uint64_t>(sc.hi, info->dlpi_addr + p.p_vaddr + p.p_memsz);
      }
      return 1;
    }, &scan);
    if (scan.lo != GAME_IMAGE_BASE) host::die("%s did not load at %llX; it must be the Linux build made with the fixed-address linker script", g_dll.c_str(), (unsigned long long)GAME_IMAGE_BASE);
    host::game_image = (uint8_t*)GAME_IMAGE_BASE;
    host::game_image_size = (uint32_t)(scan.hi - scan.lo);
  }
#else
  // In an AppContainer the host maps the image itself into the range held
  // since startup (g_game_image_reservation) and keeps the hold; elsewhere the loader needs it free.
  const bool sandboxed = in_app_container();
  // MELEE_TEST_RELOCATED_GAME_IMAGE=1 (tests): the range stays held, so Windows places the library
  // elsewhere, as a relocation policy does, and the mapping below is what loads it.
  const bool test_relocated = host::test_env::getenv("MELEE_TEST_RELOCATED_GAME_IMAGE") != nullptr;
  if (!sandboxed && !test_relocated) g_game_image_reservation.release();
  std::string map_error;
  HMODULE module = sandboxed ? map_game_image(g_dll, &map_error) : LoadLibraryA(g_dll.c_str());
  if (!module && sandboxed) host::die("cannot map %s: %s", g_dll.c_str(), map_error.c_str());
  if (!module) {
    const DWORD error = GetLastError();
    // ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION (4551) is an App Control refusal.
    if (error == 4551)
      host::die("Windows blocked %s (Smart App Control or an app control policy, error 4551). "
                "You can check Smart App Control under App & browser control in Windows Security. "
                "On a managed PC, contact your administrator.", g_dll.c_str());
    host::die("cannot load %s (error %lu)", g_dll.c_str(), error);
  }
  if (sandboxed) host::log("source port: %s mapped at its base by the host (sandboxed)", g_dll.c_str());
  // Windows can put the library somewhere else although its range is free: Exploit protection's
  // "Force randomization for images" (Mandatory ASLR) relocates every image, and it landed at a new
  // low address each start (1.0.2 reports: 0x70000, 0xE0000, 0x580000). Then the host maps it at its
  // base itself, as it does in an AppContainer, in the range taken back for it.
  bool mapped = sandboxed;
  if (!sandboxed && (uintptr_t)module != GAME_IMAGE_BASE) {
    host::log("source port: Windows loaded %s at %p, not at its base (a relocation policy); mapping it there instead",
              g_dll.c_str(), (void*)module);
    FreeLibrary(module);
    if (!g_game_image_reservation.base)
      g_game_image_reservation.base = VirtualAlloc((void*)GAME_IMAGE_BASE, 0x04000000u, MEM_RESERVE, PAGE_NOACCESS);
    module = g_game_image_reservation.base ? map_game_image(g_dll, &map_error) : nullptr;
    if (!module)
      host::die("%s could not be placed at %llX (%s)", g_dll.c_str(), (unsigned long long)GAME_IMAGE_BASE,
                g_game_image_reservation.base ? map_error.c_str() : "its address range is in use");
    mapped = true;
    host::log("source port: %s mapped at its base by the host", g_dll.c_str());
  }
  if ((uintptr_t)module != GAME_IMAGE_BASE) host::die("%s loaded at %p, not at %llX", g_dll.c_str(), (void*)module, (unsigned long long)GAME_IMAGE_BASE);
  const auto* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)module + ((const IMAGE_DOS_HEADER*)module)->e_lfanew);
  host::game_image = (uint8_t*)module;
  host::game_image_size = nt->OptionalHeader.SizeOfImage;
#endif
#ifdef _WIN32
  auto game_proc = [&](const char* name) { return mapped ? image_export(module, name) : GetProcAddress(module, name); };
#else
  auto game_proc = [&](const char* name) { return GetProcAddress(module, name); };
#endif
#endif
  auto entry = (MuGameEntry)game_proc("mu_game_entry");
#ifdef __APPLE__
  g_loaded_game_path = g_dll; // The custom ELF mapper owns this exact resources path.
#else
#ifdef _WIN32
  char loaded_path[MAX_PATH]{};
  if (GetModuleFileNameA(module,loaded_path,MAX_PATH)) g_loaded_game_path = loaded_path;
  // The sandbox's custom mapper is not in the loader list, and the working directory is the data
  // folder: the handoff identity and the snapexcl must read the file the mapper read.
  else g_loaded_game_path = game_library_file(g_dll);
#else
  Dl_info game_info{};
  if (entry && dladdr(reinterpret_cast<void*>(entry), &game_info) && game_info.dli_fname)
    g_loaded_game_path = game_info.dli_fname;
#endif
#endif
  // TM-CE's data files beside the game library, before the game starts (it opens them at its first menu).
  if (!g_loaded_game_path.empty()) publish_training_files(g_loaded_game_path.parent_path());
  user_gecko::set_native_writer((user_gecko::NativeWrite)game_proc("mu_user_gecko_write"));
  user_gecko::set_native_reader((user_gecko::NativeRead)game_proc("mu_user_gecko_read"));
  user_gecko::set_agreed_writer((user_gecko::NativeWrite)game_proc("mu_user_gecko_write_agreed"),
                                (user_gecko::NativeRead)game_proc("mu_user_gecko_read_agreed"));
  {
    // Functions a code may make return at once (shim/mu_gecko_functions.c); an older library has none.
    using Functions = uint32_t (*)(const user_gecko::NativeFunction**);
    const auto functions = (Functions)game_proc("mu_user_gecko_functions");
    const user_gecko::NativeFunction* table = nullptr;
    const uint32_t count = functions ? functions(&table) : 0;
    user_gecko::set_native_functions(table, count, host::dol_data_read);
  }
  if (!entry) host::die("%s has no mu_game_entry", g_dll.c_str());
  static MuHostApi api = make_host();
  // The game library and this program come from the same release and refuse each other otherwise.
  // A mismatch is a partly updated install (one report: a 0.8.95 program with an older library
  // left in place), so the stop says what to do; the start of the line is kept for grouping.
  if (entry(&api, &g_game) != 0)
    host::die("%s refused host API version %u: it is from another version of " PRODUCT_NAME " than %s. "
              "Close the game, then extract the whole release zip over the folder again", g_dll.c_str(), api.version, MELEE_PORT_VERSION);
  if (g_game.version != MU_GAME_API_VERSION || !g_game.state_snapshot || !g_game.state_regions ||
      !g_game.state_exclusions || !g_game.snapshot_ranges || !g_game.practice)
    host::die("%s has game API version %u, expected %u: it is from another version of " PRODUCT_NAME " than %s. "
              "Close the game, then extract the whole release zip over the folder again", g_dll.c_str(), g_game.version, MU_GAME_API_VERSION, MELEE_PORT_VERSION);
  {
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    // Squad Strike is in both the program and the library or in neither, at the same version.
    auto squad_build = (int (*)())game_proc("mu_squad_build");
#if MU_SQUAD
    if (!squad_build || squad_build() != MU_SQUAD_VERSION)
#else
    if (squad_build)
#endif
      host::die("%s and %s disagree on Squad Strike: it is from another build of " PRODUCT_NAME ". "
                "Close the game, then extract the whole release zip over the folder again", g_dll.c_str(), MELEE_PORT_VERSION);
  }
  slippi::native_practice::set_native_bridge(console_practice_bridge);
  slippi::native_practice::set_content_bridge(prepare_practice_content);
  slippi::online::set_plain_opponent_handler(plain_opponent);
  MuStateRegion game_regions[3]{};
  std::string region_error;
  const uint32_t region_count = g_game.state_regions(game_regions, 3);
  if (!validate_state_regions(game_regions, region_count, MEM1_BASE, MEM1_SIZE, &region_error))
    host::die("%s reported invalid native state regions: %s", g_dll.c_str(), region_error.c_str());
  host::log("source port: native state regions MEM1 %u, data %u, bss %u bytes",
            game_regions[0].size, game_regions[1].size, game_regions[2].size);
  std::vector<MuStateRegion> exclusions(g_game.state_exclusions(nullptr, 0));
  const uint32_t exclusion_count = g_game.state_exclusions(exclusions.data(), (uint32_t)exclusions.size());
  if (exclusion_count != exclusions.size() ||
      !validate_state_exclusions(game_regions, 3, exclusions.data(), exclusion_count, &region_error))
    host::die("%s reported invalid native state exclusions: %s", g_dll.c_str(),
              exclusion_count != exclusions.size() ? "count changed" : region_error.c_str());
  uint64_t excluded_bytes = 0;
  for (const auto& e : exclusions) excluded_bytes += e.size;
  host::log("source port: native state exclusions %u (%llu bytes)", exclusion_count,
            (unsigned long long)excluded_bytes);
  host::native_retrace = native_retrace;
  host::native_state_snapshot = g_game.state_snapshot;
#ifdef _WIN32
  AddVectoredExceptionHandler(1, on_game_exception);
#else
  install_game_signal_handler();
#endif
  gx::set_native_owner_source(true);
  // The L-cancel helper reads the native game's fighters through the game, not guest memory.
  lcancel::set_native_view([](MuLcancelView* out) { if (g_game.lcancel_view) g_game.lcancel_view(out); });
  host::log("source port: %s at %p, MEM1 %u MB at %08llX", g_dll.c_str(), (void*)module, MEM1_SIZE >> 20, (unsigned long long)MEM1_BASE);
  int code = 0;
#ifndef MELEE_NO_SLIPPI
  slippi::spectate::start();   // Slippi Dolphin's spectator server (settings: Spectator server)
#endif
  // Opponents' skins: a costume the mod browser installs keeps its SSBM Nucleus ids and the downloaded
  // file's hash, which is what this machine announces for it online (opponent_skins.h record_origin).
  host::mod_browser::set_skin_installed([](const std::string& asset_id, int mod_id, int file_id, const std::string& path,
                                           const std::string& archive_sha256) {
    if (mod_id > 0 && file_id > 0) host::skins::record_origin(asset_id, (uint32_t)mod_id, (uint32_t)file_id, path, archive_sha256);
  });
#ifdef __APPLE__
  g_mac_running = true;
  if (setjmp(g_mac_run_boundary) == 0) guarded([&] { code = g_game.run(); });
  else code = g_exit_code;
  g_mac_running = false;
#else
#ifdef _WIN32
  // Fighter plugins (plugin_gate.h): the mod browser's note after an install says whether the plugin
  // will load. In an interactive start, a plugin nobody has decided on yet is put to the player here,
  // before the game starts; a hidden or --no-dialogs run never asks and just leaves it out.
  host::mod_browser::set_plugin_check(plugin_gate::would_load);
  if (!host::g_no_dialogs && !host::test_env::automated) gated_plugin_folder(true);
#endif
  guarded([&] { code = g_game.run(); });
#ifdef _WIN32
  plugin_gate::end_run();
#endif
#endif
  // An orderly window close before Game End is a quit, sampled by the game before unloading.
  finish_tournament_before_exit();
  rollback_log(true);
  log_native_pose_bridge_stats();
  gx::set_native_pose_capture_enabled(false);
  host::log("game returned %d", code);
  return code;
}

}  // namespace source_port
