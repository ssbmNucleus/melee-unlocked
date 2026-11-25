// Native Melee port entry point.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "test_env.h"
#include "public_build.h"   // NUCLEUS_PUBLIC_BUILD: test-only options left out of --help and refused
#include <functional>
#include <iterator>
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>   // CommandLineToArgvW: the options read before the C++ initialisers run
#include <tlhelp32.h>
#include <psapi.h>
#include "replay_bar.h"
#include "host.h"
#include "sysgen_boot.h"
#include "gameplay_policy.h"
#include "gecko_data.h"
#include "render_observer.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#include "source_p2p.h"      // --p2p-*: a peer-to-peer match from the command line
#else
#include "slippi_playback.h"
#include "exi_slippi.h"
#include "slippi_online.h"
#ifndef MELEE_NO_SLIPPI
#endif
#include "slippi_net.h"
#include "jukebox.h"
// The Source Port host with the Slippi layer also runs the project's own peer-to-peer session when
// the command line asks for one (--p2p-*); with no such option nothing of it is used.
#include "netplay_state.h"
#include "source_p2p.h"
#endif
#include "audio.h"
#include "guest_registry.h"
#include "gx_vulkan.h"
#include "gx_backend.h"
#include "settings_log.h"
#include "gx_core.h"
#include "pipeline_cache_store.h"
#include "prewarm_only.h"
#include "render_options.h"
#include "gx_retro.h"
#include "pc_settings.h"
#include "texture_pack.h"
#include "cosmetic_mods.h"
#include "menu_mods.h"
#include "new_stages.h"
#include "skin_fetch.h"   // opponents' skins: the download cache folder
#include "opponent_skins.h"   // ... and whether they are on, for the log
#include "disc_skin_scan.h"
#include "hackpack_ai.h"
#include "practice_cpu.h"
#include "mod_scan.h"
#include "replay_slpz.h"
#include "threaded_backend.h"
#include "window.h"
#include "lcancel.h"
#include "user_gecko.h"
#include "optional_downloads.h"
#include "discord_presence.h"
#include "replay_control.h"
#include "product_identity.h"   // the product named by --print-info
#include "crash_report.h"
#include "sim_profiler.h"
#include "early_start.h"
#include "replay_args.h"
#include "cosmetics_cli.h"
namespace app { int run_settings_window(gx::RenderOptions& options, unsigned long standby_parent, std::function<void()> reload); }
#include "source_host.h"
#ifdef _WIN32
#include "plugin_gate.h"   // fighter plugin approval: --approve-plugin, --list-plugins, --revoke-plugin, --print-info
#endif
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <exception>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>


enum class CliArgument { Flag, Value, ValueBeforeSettings, OptionalValue, OptionalEquals };

struct CliOption {
  const char* name;
  CliArgument argument;
  const char* help;
  // Empty for one-shot commands that dispatch before game startup.
  std::function<int(const std::string&, const char*)> apply = {};
  std::function<void(const char*)> prepare = {};
  // A test fixture's entry point: a public build (public_build.h) leaves it out of --help and refuses it.
  bool test_only = false;
};

// A handler either continues parsing, asks for help and exit 2, or returns an exit code.
static constexpr int kParseNext = -1;
static constexpr int kParseUsage = -2;

template <size_t N>
static void print_usage(const CliOption (&options)[N]) {
  std::printf("%s %s, a fork of %s\n\n", product::kName, MELEE_PORT_VERSION, product::kUpstreamName);
  std::printf("Usage: melee_source [options]\n\n");
  for (const auto& option : options)
    if (!(option.test_only && host::public_build::kOn)) std::printf("  %s%s\n", option.name, option.help);
  std::printf("\n");
  // The list in host.h (kDieExit) is the same; keep the two together.
  std::printf("Exit codes: 0 ended normally (Quit, --frames reached, --help, --version, --print-info)\n"
              "            1 could not start (the disc, a replay or an input script could not be opened, memory could not be reserved)\n"
              "            2 bad command line; a disc this launch cannot use (a modded disc where the online menus need the\n"
              "              original, a disc that is not retail 1.02 with --vanilla-only); a lobby/online test match that never started\n"
              "            3 this processor lacks AVX2, which this build needs\n"
              "            4 a replay was closed before its end (a queue of replays can stop there)\n"
              "            5 a replay this game cannot play was refused before its match (a fighter, stage or costume it\n"
              "              names is not in this game, or it was recorded with other installed fighters; the log says which)\n"
              "            70 the game stopped on an error it reported (melee_port_crash.txt and the log say which)\n"
              "            an exception code (0xC0000005 ...) after a crash the crash handler wrote up\n");
}


// Windows hands out ~15.6 ms timer granularity by default, so every pacing sleep (the 60 Hz
// retrace, the presentation deadline, the audio device wait) overshoots by up to a frame. One
// millisecond is what games ask for, and it is what makes 60 Hz land on 60 Hz.
struct TimerResolution {
  bool raised = timeBeginPeriod(1) == TIMERR_NOERROR;
  ~TimerResolution() { if (raised) timeEndPeriod(1); }
};

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

static int melee_main(int argc, char** argv);

// Both entry points exist so the executable links whichever subsystem it is built for: WinMain for the
// windowed build (no terminal alongside the game), main if it is ever built as a console program.
static int guarded_main(int argc, char** argv) {
  // The same for an exception that escapes any other thread: the C++ runtime ends the program
  // there (terminate, then abort) without passing through the crash report. The terminate handler
  // is per thread, the abort signal is not, so the report is written from both.
  std::set_terminate([] { report_unhandled(); });
#ifdef _WIN32
  std::signal(SIGABRT, [](int) { report_unhandled(); });
#else
  std::signal(SIGABRT, [](int) { linux_abort_note(); report_unhandled(); });
#endif
  try {
    // A run that returns here (a frame limit, an option that ends the run, a bad option, a disc that
    // cannot be opened, a skin catalog command) must not go on to the C library's exit. That destroys
    // the global std::thread objects of threads still running (Discord, texture packs, adapters,
    // servers) and std::terminate aborts the game at the very end, with a crash report and, on
    // Windows, a message box: a bad option once left one waiting on the screen. end_process ends it
    // with the code melee_main returned instead.
    host::end_process(melee_main(argc, argv));
  } catch (const std::exception& error) {
    host::die("the game stopped on an internal error: %s", error.what());
  }
}
#ifdef _WIN32
#include "standalone_launch_win.h"
// A DLL loaded by name is looked for beside the executable, in System32 and in folders added on
// purpose (AddDllDirectory, SetDllDirectory), never in the current folder or on PATH, where a file
// of the same name from somewhere else would be taken. The game's own optional DLLs (DLSS, FSR, the
// pro AI) are loaded by full path.
static const bool g_dll_search_restricted = SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
    LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) != FALSE;
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  attach_parent_console();
  // A plain double-click (no arguments at all): find the player's disc (asking once) and start the game with it.
  if (__argc == 1) return standalone_launch::run();
  return guarded_main(__argc, __argv);
}
#endif
#ifdef __APPLE__
#include "../mac/full_launch.h"
int main(int argc, char** argv) {
  try {
    auto args = mac_host::full_launch_arguments(argc, argv);
    if (args.empty()) return 0; // Cancelled the disc picker.
    std::vector<char*> pointers;
    for (auto& arg : args) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    return guarded_main((int)args.size(), pointers.data());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    if (argc == 1) mac_host::show_launch_error(error.what());
    return 2;
  }
}
#else
int main(int argc, char** argv) { return guarded_main(argc, argv); }
#endif

// Resolve the window API; graphics-only runs never read or change crashed-start recovery.
static void prepare_render_backend(gx::RenderOptions& gfx) {
  if (gfx.api == gx::RenderApi::Vulkan && !gx::vulkan_available()) {
    host::log("vulkan: not usable on this system; starting on the default renderer");
    gfx.api = gx::RenderApi::D3D12;
  }
  // A Linux driver fault cannot be caught by the renderer. Recover on the next launch instead of
  // making the player repeat the same crash. Windows first retries with a cleared shader cache.
#ifdef __linux__
  constexpr int vulkan_failure_limit = 1;
#else
  constexpr int vulkan_failure_limit = 2;
#endif
  gx::vulkan_set_shader_cache(gfx.shader_cache);   // the start marker lives in the cache folder
  if (!gfx.prewarm_only && gfx.api == gx::RenderApi::Vulkan && gx::vulkan_failed_starts() >= vulkan_failure_limit) {
    host::log("vulkan: the last %d starts did not finish; starting on the default renderer (pick Vulkan in the settings to try it again)",
              gx::vulkan_failed_starts());
    gfx.api = gx::RenderApi::D3D12;
  }
  host::window_prefer_vulkan(gfx.api == gx::RenderApi::Vulkan);
}

static int run_prewarm_only(int argc, char** argv, const std::function<void()>& usage) {
  crash_set_dialog(false);
#ifdef _WIN32
  SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#else
  app::prewarm_only::install_crash_handlers();
#endif
  host::options.log_file = app::prewarm_only::log_path({});
  try {
    return app::prewarm_only::with_arguments(argc, argv, [&](const app::prewarm_only::Arguments& args) {
      host::options.log_file = app::prewarm_only::log_path(args.log_file);
      host::options.no_gc_adapter = true;
      if (!args.sys_dir.empty()) host::options.sys_dir = args.sys_dir;
      host::set_die_hook(app::prewarm_only::die_hook<host::end_process>);
      gx::RenderOptions gfx;
      gfx.prewarm_only = true;
      gfx.native_source = true;
      if (!args.settings_path.empty()) gfx.settings_path = args.settings_path;
      int volume = 0;
      gx::load_pc_settings(gfx, volume);
      if (!args.shader_cache.empty()) { gfx.shader_cache = args.shader_cache; gfx.shader_cache_shared = true; }
      if (!args.backend.empty()) {
        if (args.backend == "vulkan" || args.backend == "vk") gfx.api = gx::RenderApi::Vulkan;
        else if (args.backend == "d3d11" || args.backend == "dx11" || args.backend == "11") gfx.api = gx::RenderApi::D3D11;
        else if (args.backend == "d3d12" || args.backend == "dx12" || args.backend == "12") gfx.api = gx::RenderApi::D3D12;
        else { host::log("prewarm-only: unknown backend"); usage(); return app::prewarm_only::usage_error; }
      }
      if (gfx.api == gx::RenderApi::D3D11) {
        host::log("prewarm-only: unsupported backend: d3d11 has no recipe prewarm");
        return app::prewarm_only::unsupported_backend;
      }
      gfx.pc_settings = false;
      gfx.fullscreen = gfx.exclusive_fullscreen = false;
      gfx.slippi_spectator = false;
      host::window_prefer_background(true);
      prepare_render_backend(gfx);
      // Since v0.9.104 the Vulkan cache sits under the shader cache folder (vulkan_set_shader_cache), as a
      // normal launch with the same --shader-cache keeps it: lock that folder.
      const auto root = std::filesystem::u8path(gfx.shader_cache.empty() ? "shadercache" : gfx.shader_cache);
      const auto cache = gfx.api == gx::RenderApi::Vulkan ? root / "vulkan-v1" : root;
      const int result = app::prewarm_only::with_cache_lock(cache, [&] {
        const int status = app::prewarm_only::run([&] {
          void* window = host::window_create(gfx.window_w, gfx.window_h, L"" PRODUCT_NAME " prewarm", false
#ifdef _WIN32
              , true
#endif
          );
          if (!window) return std::unique_ptr<gx::Backend>{};
          return std::unique_ptr<gx::Backend>(gx::create_render_backend(window, gfx.window_w, gfx.window_h, gfx));
        }, [] {
          return ::gx::cache_store::flush_all_result("prewarm-only", std::chrono::seconds(30));
        }, [](const gx::PrewarmStats& stats, long long ms, app::prewarm_only::SaveResult saved) {
          host::log("prewarm-only: backend=%s recipes=%llu cache_hits=%llu compiled=%llu ms=%lld saved=%s",
                    stats.backend, (unsigned long long)stats.recipes, (unsigned long long)stats.cache_hits,
                    (unsigned long long)stats.compiled, ms, gx::cache_store::flush_result_name(saved));
        }, [](const char* error) { host::log("prewarm-only: renderer failed: %s", error); });
        // A timed-out saver can still own the driver; end_process releases the window/device.
        if (status != app::prewarm_only::save_timeout) host::window_destroy();
        return status;
      });
      if (result == app::prewarm_only::already_running) host::log("prewarm-only: already running");
      return result;
    }, [&](const char* error) { host::log("prewarm-only: %s", error); usage(); });
  } catch (const std::exception& e) {
    host::log("prewarm-only: %s", e.what());
    return app::prewarm_only::no_renderer;
  }
}

static int melee_main(int argc, char** argv) {
  host::Options& o = host::options;
  bool headless = false, hidden = false, threaded = false, fps_requested = false;
  bool scripted = false, allow_matchmaking = false;   // automated runs stay off Slippi's servers
  std::string sysgen_user_dir, sysgen_data_dir;
  std::string card_self_test_dir;
  std::string replay_arg;   // Source engine --replay
#ifndef MELEE_NO_SLIPPI
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  bool squad_test_invalid_lineup = false;
#endif
  bool online_test_arg = false;   // --online-test was given: a Slippi test match, never with --p2p-*
#endif
  std::string cosmetic_import, cosmetic_scan_disc, cosmetic_select, cosmetic_deselect;
  std::string effect_check, effects_check, effects_csv;   // --check-effect, --check-effects, --csv
  bool cosmetic_deselect_requested = false;
  bool cosmetic_enable_effects = false, cosmetic_restore = false, cosmetic_status = false;
  gx::RenderOptions gfx;
  gfx.native_source = true;
  bool automated = false, explicit_frame_mode = false, settings_window_only = false, load_settings = false;
  bool explicit_card_dir = false;
  bool no_crash_dialog = false;   // --no-dialogs / --no-crash-dialog (usage)
  unsigned long settings_standby = 0;   // the process id of the program that keeps it: start hidden, show when it asks
  const auto accept = [](const std::string&, const char*) { return kParseNext; };
  const auto apply_p2p = [](const std::string& name, const char* value) {
    return source_p2p::option(name, value) ? kParseNext : kParseUsage;
  };
  const auto prepare_hidden = [&](const char*) {
    automated = true;
    host::test_env::automated = true;
  };
  const auto prepare_no_dialogs = [&](const char*) { no_crash_dialog = true; };
  const CliOption options[] = {
    {"-h", CliArgument::Flag, "  Show this help (alias for --help)."},
    {"--help", CliArgument::Flag, "  Show this help and exit. Use by itself."},
    {"--version", CliArgument::Flag, "  Print the bare version and exit."},
    {"--print-info", CliArgument::Flag, "  Print JSON build information, without a disc or window."},
    {"--prewarm-only", CliArgument::Flag, "  Prepare the graphics cache in the normal launch directory, without a disc.\n"
     "    Exits: 0 complete, 2 no renderer, 3 write failed, 4 already running, 5 save timeout,\n"
     "           6 unsupported backend, 64 invalid arguments."},
#ifdef _WIN32
    {"--approve-plugin", CliArgument::Value, " <file.mufighter>  Approve this exact fighter plugin and print JSON."},
    {"--revoke-plugin", CliArgument::Value, " <sha256>  Revoke approval for this hash and print JSON."},
    {"--list-plugins", CliArgument::Flag, "  List fighter plugins and approvals as JSON."},
#endif
    {"--iso", CliArgument::Value, " <path>  Open a Melee NTSC 1.02 disc.",
     [&](const std::string&, const char* value) { o.iso = value; return kParseNext; }},
    {"-e", CliArgument::Value, " <path>  Alias for --iso (Dolphin spelling).",
     [&](const std::string&, const char* value) { o.iso = value; return kParseNext; }},
    {"--mod-base-iso", CliArgument::Value, " <path>  Use this retail disc as the base for the mod disc in --iso.",
     [&](const std::string&, const char* value) { o.mod_base_iso = value; return kParseNext; }},
    {"--look-iso", CliArgument::Value, " <path>  Play the standard game with this disc's looks.",
     [&](const std::string&, const char* value) { o.look_iso = value; return kParseNext; }},
    {"--frames", CliArgument::Value, " <count>  Exit after this many frames.",
     [&](const std::string&, const char* value) { o.frames = (uint32_t)std::strtoul(value, nullptr, 0); return kParseNext; }},
    {"--fast", CliArgument::Flag, "  Run without normal frame pacing.",
     [&](const std::string&, const char*) { o.fast = true; return kParseNext; }},
    {"--headless", CliArgument::Flag, "  Run without a renderer or window.",
     [&](const std::string&, const char*) { headless = true; return kParseNext; }, prepare_hidden},
    {"--hidden", CliArgument::Flag, "  Run with the game window hidden.",
     [&](const std::string&, const char*) { hidden = true; return kParseNext; }, prepare_hidden},
    {"--threaded-renderer", CliArgument::Flag, "  Run the renderer on its own thread.",
     [&](const std::string&, const char*) { threaded = true; return kParseNext; }},
    {"--fps", CliArgument::Value, " <N|monitor|unlocked>  Set the display rate (requires a frame mode other than off).",
     [&](const std::string&, const char* value) {
       const std::string v = value;
       gfx.fps_cap = v == "unlocked" ? 0 : v == "monitor" ? -1 : std::atoi(v.c_str());
       if (v != "unlocked" && v != "monitor" &&
           (gfx.fps_cap < 1 || v.find_first_not_of("0123456789") != std::string::npos))
         return kParseUsage;
       threaded = true;
       fps_requested = true;
       return kParseNext;
     }},
    {"--frame-mode", CliArgument::Value, " <extrapolate|interpolate|authored|authored-interpolate|off>  Select subframe animation.",
     [&](const std::string&, const char* value) {
       std::string v = value;
       if (v == "extrapolate") gfx.subframe = gx::SubFrameMode::Extrapolate;
       else if (v == "interpolate") gfx.subframe = gx::SubFrameMode::Interpolate;
       else if (v == "authored") gfx.subframe = gx::SubFrameMode::Authored;
       else if (v == "authored-interpolate") gfx.subframe = gx::SubFrameMode::AuthoredInterpolate;
       else if (v == "off") gfx.subframe = gx::SubFrameMode::Off;
       else return kParseUsage;
       threaded = true;
       return kParseNext;
     }, [&](const char*) { explicit_frame_mode = true; }},
    {"--scale", CliArgument::Value, " <N|auto>  Set the internal resolution scale.",
     [&](const std::string&, const char* value) {
       const bool automatic = std::string_view(value) == "auto";
       gfx.efb_scale = automatic ? 0 : std::atoi(value);
       if (!automatic && gfx.efb_scale < 1) return kParseUsage;
       return kParseNext;
     }},
    {"--window", CliArgument::Value, " <WxH>  Set the window size.",
     [&](const std::string&, const char* value) {
       if (std::sscanf(value, "%dx%d", &gfx.window_w, &gfx.window_h) != 2 || gfx.window_w < 320 || gfx.window_h < 240)
         return kParseUsage;
       gfx.window_pinned = true;
       return kParseNext;
     }},
    {"--aspect", CliArgument::Value, " <auto|73:60|native|4:3|16:9|stretch>  Set the presentation aspect ratio.",
     [&](const std::string&, const char* value) {
       std::string v = value;
       gfx.aspect = v == "auto" ? gx::AspectMode::Auto : v == "73:60" || v == "native" ? gx::AspectMode::Native
                  : v == "4:3" ? gx::AspectMode::Force4_3 : v == "16:9" ? gx::AspectMode::Force16_9
                  : v == "stretch" ? gx::AspectMode::Stretch : (gx::AspectMode)-1;
       if ((int)gfx.aspect < 0) { std::fprintf(stderr, "--aspect auto|73:60|4:3|16:9|stretch\n"); return 2; }
       return kParseNext;
     }},
    {"--fullscreen", CliArgument::Flag, "  Start fullscreen.",
     [&](const std::string&, const char*) { gfx.fullscreen = true; return kParseNext; }},
    {"--backend", CliArgument::Value, " <d3d12|d3d11|vulkan>  Select the graphics backend.",
     [&](const std::string&, const char* value) {
       std::string v = value;
       if (v == "d3d11" || v == "dx11" || v == "11") gfx.api = gx::RenderApi::D3D11;
       else if (v == "vulkan" || v == "vk") gfx.api = gx::RenderApi::Vulkan;
       else if (v == "d3d12" || v == "dx12" || v == "12") gfx.api = gx::RenderApi::D3D12;
       else { std::fprintf(stderr, "--backend d3d12|d3d11|vulkan\n"); return 2; }
       return kParseNext;
     }},
    {"--dlss", CliArgument::Value, " <mode>  Use off, dlaa, quality, balanced, performance, ultra or an fsr-* mode.",
     [&](const std::string&, const char* value) {
       std::string v = value; gfx.dlss_mode = v == "off" ? 0 : v == "dlaa" ? 1 : v == "quality" ? 2 : v == "balanced" ? 3 : v == "performance" ? 4 : v == "ultra" ? 5 :
                                   v == "fsr-aa" ? 11 : v == "fsr-quality" ? 12 : v == "fsr-balanced" ? 13 : v == "fsr-performance" ? 14 : v == "fsr-ultra" ? 15 : -1;
       if (gfx.dlss_mode < 0) { std::fprintf(stderr, "--dlss off|dlaa|quality|balanced|performance|ultra\n"); return 2; }
       return kParseNext;
     }},
    {"--dlss-mode", CliArgument::Value, " <0..5>  Select a DLSS mode by index.",
     [&](const std::string&, const char* value) {
       char* end = nullptr; long mode = std::strtol(value, &end, 10);
       if (!end || *end || mode < 0 || mode > 5) { std::fprintf(stderr, "--dlss-mode must be 0..5\n"); return 2; }
       gfx.dlss_mode = (int)mode;
       return kParseNext;
     }},
    {"--frame-generation", CliArgument::OptionalEquals, "[=2x|3x|4x|5x|6x|dynamic]  Generate frames (2x by default).",
     [&](const std::string&, const char* value) {
       if (!value) {
         gfx.frame_generation_mode = 1; // backward-compatible 2x shorthand
       } else {
         const std::string v = value;
         gfx.frame_generation_mode = v == "2x" ? 1 : v == "3x" ? 2 : v == "4x" ? 3 :
                                     v == "dynamic" ? 4 : v == "5x" ? 5 : v == "6x" ? 6 : -1;
         if (gfx.frame_generation_mode < 0) {
           std::fprintf(stderr, "--frame-generation=2x|3x|4x|5x|6x|dynamic\n");
           return 2;
         }
       }
       return kParseNext;
     }},
    {"--ssao", CliArgument::Value, " <0..1>  Set screen-space ambient occlusion strength.",
     [&](const std::string&, const char* value) {
       char* end = nullptr; float amount = std::strtof(value, &end);
       if (!end || *end || !std::isfinite(amount) || amount < 0.0f || amount > 1.0f) { std::fprintf(stderr, "--ssao must be 0..1\n"); return 2; }
       gfx.screen_space_ao = amount;
       return kParseNext;
     }},
    {"--reflex", CliArgument::Flag, "  Use NVIDIA Reflex with Boost.",
     [&](const std::string&, const char*) { gfx.reflex_mode = 2; return kParseNext; }},
    {"--dlss-jitter-sign", CliArgument::Value, " <number>  Override the DLSS jitter sign.",
     [&](const std::string&, const char* value) { gfx.dlss_jitter_sign = (float)std::atof(value); return kParseNext; }},
    {"--vsync", CliArgument::Flag, "  Enable vertical sync for this run.",
     [&](const std::string&, const char*) { gfx.vsync = true; gfx.vsync_auto = false; return kParseNext; }},
    {"--flicker-scan", CliArgument::Flag, "  Enable the flicker scan.",
     [&](const std::string&, const char*) { gfx.flicker_scan = true; return kParseNext; }},
    {"--pin-phase", CliArgument::Value, " <number>  Pin the subframe phase.",
     [&](const std::string&, const char* value) { gfx.pin_phase = std::atof(value); return kParseNext; }},
    {"--frame-times", CliArgument::Value, " <file.csv>  Write presentation timings.",
     [&](const std::string&, const char* value) { gfx.frame_times = value; return kParseNext; }},
    {"--capture", CliArgument::Value, " <file.ppm>  Set the frame capture output.",
     [&](const std::string&, const char* value) { gfx.capture_path = value; return kParseNext; }},
    {"--capture-frame", CliArgument::Value, " <frame>  Capture a presented frame.",
     [&](const std::string&, const char* value) { gfx.capture_frame = (uint32_t)std::strtoul(value, nullptr, 0); return kParseNext; }},
    {"--capture-every", CliArgument::Value, " <frames>  Capture at a fixed interval.",
     [&](const std::string&, const char* value) { gfx.capture_every = (uint32_t)std::strtoul(value, nullptr, 0); return kParseNext; }},
    {"--capture-burst", CliArgument::Value, " <count>  Capture a burst of consecutive frames.",
     [&](const std::string&, const char* value) { gfx.capture_burst = (uint32_t)std::strtoul(value, nullptr, 0); return kParseNext; }},
    {"--capture-sim-frame", CliArgument::Value, " <frame>  Capture at a simulation frame.",
     [&](const std::string&, const char* value) { gfx.capture_sim_frame = std::strtoull(value, nullptr, 0); return kParseNext; }},
    {"--dump", CliArgument::Value, " <path>  Set the graphics dump output.",
     [&](const std::string&, const char* value) { gfx.dump_path = value; return kParseNext; }},
    {"--dump-frame", CliArgument::Value, " <frame>  Select the frame to dump.",
     [&](const std::string&, const char* value) { gfx.dump_frame = (uint32_t)std::strtoul(value, nullptr, 0); return kParseNext; }},
    {"--shader-cache", CliArgument::Value, " <path>  Use the specified shared shader cache.",
     [&](const std::string&, const char* value) { gfx.shader_cache = value; gfx.shader_cache_shared = true; return kParseNext; }},
    {"--widescreen", CliArgument::Flag, "  Enable the native 16:9 game code.",
     [&](const std::string&, const char*) { gfx.widescreen = true; gfx.true_widescreen = false; return kParseNext; }},
    {"--true-widescreen", CliArgument::OptionalEquals, "[=16:9|16:10|fit|W:H]  Widen the renderer's frustum (16:9 by default).",
     [&](const std::string&, const char* value) {
       gfx.true_widescreen = true; gfx.widescreen = false;
       const std::string v = value && *value ? value : "16:9";
       int w = 0, h = 0;
       if (v == "16:9") gfx.true_aspect = gx::TrueAspect::Wide16_9;
       else if (v == "16:10") gfx.true_aspect = gx::TrueAspect::Wide16_10;
       else if (v == "fit") gfx.true_aspect = gx::TrueAspect::FitWindow;
       else if (std::sscanf(v.c_str(), "%d:%d", &w, &h) == 2 && w >= 1 && h >= 1 && w <= 100 && h <= 100) {
         gfx.true_aspect = gx::TrueAspect::Custom; gfx.true_custom_w = w; gfx.true_custom_h = h;
       } else { std::fprintf(stderr, "--true-widescreen=16:9|16:10|fit|W:H\n"); return 2; }
       return kParseNext;
     }},
    {"--custom-textures", CliArgument::Flag, "  Load custom textures.",
     [&](const std::string&, const char*) { gfx.custom_textures = true; return kParseNext; }},
    {"--dump-textures", CliArgument::Flag, "  Dump game textures.",
     [&](const std::string&, const char*) { gfx.dump_textures = true; return kParseNext; }},
    {"--sharpness", CliArgument::Value, " <0..1>  Set output sharpness.",
     [&](const std::string&, const char* value) { gfx.sharpness = std::clamp((float)std::atof(value), 0.0f, 1.0f); return kParseNext; }},
    {"--crt", CliArgument::Value, " <0..2>  Select the CRT filter.",
     [&](const std::string&, const char* value) { gfx.crt_filter = std::clamp(std::atoi(value), 0, 2); return kParseNext; }},
    {"--border", CliArgument::Value, " <file>  Use border artwork (sharp:<file> disables its soft edge).",
     [&](const std::string&, const char* value) {
       gfx.border_art = value; if (gfx.border_art.rfind("sharp:", 0) == 0) { gfx.border_edge = 0; gfx.border_art.erase(0, 6); }
       return kParseNext;
     }},
    {"--shader", CliArgument::Value, " <file>  Use a shader preset.",
     [&](const std::string&, const char* value) { gfx.shader_preset = value; return kParseNext; }},
    {"--ssaa", CliArgument::Value, " <1..4>  Set supersampling.",
     [&](const std::string&, const char* value) { gfx.ssaa = std::clamp(std::atoi(value), 1, 4); return kParseNext; }},
    {"--msaa", CliArgument::Value, " <1|2|4|8>  Set multisampling.",
     [&](const std::string&, const char* value) { const int n = std::atoi(value); gfx.msaa = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; return kParseNext; }},
    {"--anisotropy", CliArgument::Value, " <1..16>  Set texture anisotropy.",
     [&](const std::string&, const char* value) { gfx.anisotropy = std::clamp(std::atoi(value), 1, 16); return kParseNext; }},
    {"--effects", CliArgument::Value, " <0..3>  Set the graphics effects level.",
     [&](const std::string&, const char* value) { gfx.effects_level = std::clamp(std::atoi(value), 0, 3); return kParseNext; }},
    {"--lightweight", CliArgument::Value, " <index>  Apply a Lightweight preset.",
     [&](const std::string&, const char* value) { gx::apply_lightweight(gfx, std::atoi(value), false); return kParseNext; }},
    {"--look", CliArgument::Value, " <index>  Apply a look preset.",
     [&](const std::string&, const char* value) { gx::apply_look_preset(gfx, std::atoi(value), false); return kParseNext; }},
    {"--retro", CliArgument::Flag, "  Use the Retro look.",
     [&](const std::string&, const char*) { gfx.retro = true; return kParseNext; }},
    {"--settings-window", CliArgument::Flag, "  Open the standalone Settings window.", accept,
     [&](const char*) { settings_window_only = true; }},
    {"--settings-path", CliArgument::ValueBeforeSettings, " <file>  Use this settings file and the profiles beside it.",
     [&](const std::string&, const char* value) { gfx.settings_path = value; return kParseNext; },
     [&](const char* value) { gfx.settings_path = value; }},
    {"--settings-tab", CliArgument::ValueBeforeSettings, " <video|audio|game|controls|overlays|gecko>  Choose the Settings page.", accept,
     [](const char* value) {
       // Pages 5 (Customize) and 7 (Mods) were removed. Keep the current page numbers.
       static constexpr struct { const char* name; int page; } tabs[] = {
         {"video", 0}, {"audio", 1}, {"game", 2}, {"controls", 3}, {"overlays", 4}, {"gecko", 6}};
       for (const auto& tab : tabs)
         if (std::string_view(value) == tab.name) gx::settings_set_initial_tab(tab.page);
     }},
    {"--settings-standby", CliArgument::ValueBeforeSettings, " <parent PID>  Start Settings hidden for the given parent process.", accept,
     [&](const char* value) { settings_standby = std::strtoul(value, nullptr, 10); }},
    {"--load-settings", CliArgument::Flag, "  Load the settings file in a hidden run.", accept,
     [&](const char*) { load_settings = true; }},
    {"--pc-settings", CliArgument::Flag, "  Enable on-screen readouts, controller display and the Practice menu in a hidden run.",
     [&](const std::string&, const char*) { gfx.pc_settings = true; return kParseNext; }},
    {"--no-dialogs", CliArgument::Flag, "  Log errors and write crash reports without opening message boxes.", accept, prepare_no_dialogs},
    {"--no-crash-dialog", CliArgument::Flag, "  Alias for --no-dialogs.", accept, prepare_no_dialogs},
    {"--volume", CliArgument::Value, " <0..100>  Set the game audio volume.",
     [&](const std::string&, const char* value) { o.volume = std::atoi(value); return kParseNext; }},
    {"--music", CliArgument::Value, " <0..100>  Set the music volume.",
     [&](const std::string&, const char* value) { slippi::jukebox::set_user_volume(std::clamp(std::atoi(value), 0, 100)); return kParseNext; }},
    {"--no-music", CliArgument::Flag, "  Mute the music.",
     [&](const std::string&, const char*) { slippi::jukebox::set_user_volume(0); return kParseNext; }},
    {"--music-volume", CliArgument::Value, " <0..100>  Set the music slider for a scripted run.",
     [&](const std::string&, const char* value) { slippi::jukebox::set_user_volume(std::atoi(value)); return kParseNext; }},
    {"--audio-dump", CliArgument::Value, " <file.wav>  Write the audio mix.",
     [&](const std::string&, const char* value) { o.audio_dump = value; return kParseNext; }},
    {"--list-audio-devices", CliArgument::Flag, "  Print audio endpoint IDs and names and exit.",
     [&](const std::string&, const char*) {
       for (const auto& d : host::audio_list_devices()) std::printf("%s\t%s\n", d.id.c_str(), d.name.c_str());
       return 0;
     }},
    {"--sys-dir", CliArgument::Value, " <folder>  Use this system data folder.",
     [&](const std::string&, const char* value) { o.sys_dir = value; return kParseNext; }},
    {"--sysgen-data-dir", CliArgument::Value, " <folder>  Use this writable generated data folder.",
     [&](const std::string&, const char* value) { sysgen_data_dir = value; return kParseNext; }},
    {"--replay-dir", CliArgument::Value, " <folder>  Use this replay output folder.",
     [&](const std::string&, const char* value) { o.replay_dir = value; return kParseNext; }},
    {"--replay-month-folders", CliArgument::Flag, "  Save each replay in a YYYY-MM folder inside the replay folder, as Slippi's monthly subfolders.",
     [&](const std::string&, const char*) { o.replay_month_folders = true; return kParseNext; }},
    {"--compress-replays", CliArgument::Flag, "  Convert earlier .slp replays in the replay folder to .slpz.",
     [&](const std::string&, const char*) { g_compress_replays = true; return kParseNext; }},
    {"--replay", CliArgument::Value, " <file.slp>  Play a replay.",
     [&](const std::string&, const char* value) { replay_arg = playable_replay(value); return kParseNext; }},
    {"-i", CliArgument::Value, " <file>  Read the replay path from a Dolphin playback communication file.",
     [&](const std::string&, const char* value) { replay_arg = playable_replay(comm_file_replay(value)); return kParseNext; }},
    {"-b", CliArgument::Flag, "  Accept Dolphin's exit-at-end switch. Replays already exit at the end.", accept},
    {"--trace", CliArgument::Value, " <file.trace>  Read a replay's session trace.",
     [&](const std::string&, const char* value) { replay_control::set_trace_file(value); return kParseNext; }},
    {"--as-experienced", CliArgument::Flag, "  Look up the session trace beside the replay.",
     [&](const std::string&, const char*) { replay_control::set_as_experienced(true); return kParseNext; }},
    {"--card-dir", CliArgument::Value, " <folder>  Store memory cards in this folder.",
     [&](const std::string&, const char* value) { o.card_dir = value; return kParseNext; },
     [&](const char*) { explicit_card_dir = true; }},
    {"--card-self-test", CliArgument::Value, " <new scratch folder>  Run the memory card self-test and exit.",
     [&](const std::string&, const char* value) { card_self_test_dir = value; return kParseNext; }},
    {"--fighters-dir", CliArgument::Value, " <absolute folder|none>  Choose the installed-fighter and plugin folder (overrides MELEE_FIGHTER_PLUGINS).",
     [&](const std::string&, const char* value) {
       const std::string v = value;
       if (v != "none" && !std::filesystem::u8path(v).is_absolute()) {
         std::fprintf(stderr, "--fighters-dir needs an absolute folder path, or none for no installed fighters\n");
         return 2;
       }
       o.fighters_dir = v;
       o.fighters_dir_set = true;
       return kParseNext;
     }},
    {"--vanilla-only", CliArgument::Flag, "  Require retail 1.02 and suspend gameplay mods.", accept,
     [](const char*) { host::gameplay::vanilla_only_session = true; }},
    {"--vanilla-game", CliArgument::Flag, "  Run without gameplay mods or installed fighters.",
     [&](const std::string&, const char*) { o.vanilla_game = true; return kParseNext; }},
    {"--no-mod-autodetect", CliArgument::Flag, "  Disable automatic mod detection.",
     [&](const std::string&, const char*) {
       host::gameplay::mod_autodetect_disabled = true;
       source_port::mods::set_auto_detect(false);
       return kParseNext;
     }},
    {"--scan-mods", CliArgument::Flag, "  Enable automatic mod detection in a scripted run.",
     [&](const std::string&, const char*) { source_port::mods::set_auto_detect(true); return kParseNext; }},
    {"--mod-profile", CliArgument::Value, " <name>  Use this mod profile.",
     [&](const std::string&, const char* value) { if (!source_port::set_mod_profile(value)) return 2; return kParseNext; }},
    {"--mod-dir", CliArgument::Value, " <folder>  Add a disc-files folder.",
     [&](const std::string&, const char* value) { source_port::set_mod_directory(value); return kParseNext; }},
    {"--mod-iso", CliArgument::Value, " <path>  Add a patched disc as an overlay.",
     [&](const std::string&, const char* value) { source_port::set_mod_iso(value); return kParseNext; }},
    {"--mod-gci", CliArgument::Value, " <file.gci>  Add a save file.",
     [&](const std::string&, const char* value) { source_port::set_mod_gci(value); return kParseNext; }},
    {"--mod-opponent-unverified", CliArgument::Flag, "  Allow an unverified opponent when using a mod in Direct.",
     [&](const std::string&, const char*) { gx::RenderOptions::live_mods_dolphin_ok() = true; return kParseNext; }},
    {"--import-cosmetic", CliArgument::Value, " <file>  Import a costume into the catalog and exit.",
     [&](const std::string&, const char* value) { cosmetic_import = value; return kParseNext; }},
    {"--scan-disc-skins", CliArgument::Value, " <mod.iso>  List a mod disc's costumes as skins and exit.",
     [&](const std::string&, const char* value) { cosmetic_scan_disc = value; return kParseNext; }},
    {"--select-cosmetic", CliArgument::Value, " <id>  Select a catalog asset and exit.",
     [&](const std::string&, const char* value) { cosmetic_select = value; return kParseNext; }},
    {"--deselect-cosmetic", CliArgument::Value, " <slot>  Deselect that cosmetic slot and exit.",
     [&](const std::string&, const char* value) { cosmetic_deselect = value; cosmetic_deselect_requested = true; return kParseNext; }},
    {"--enable-project-effects", CliArgument::Flag, "  Enable project effects and exit.",
     [&](const std::string&, const char*) { cosmetic_enable_effects = true; return kParseNext; }},
    {"--restore-vanilla-cosmetics", CliArgument::Flag, "  Restore original cosmetics and exit.",
     [&](const std::string&, const char*) { cosmetic_restore = true; return kParseNext; }},
    {"--cosmetic-status", CliArgument::Flag, "  Print the cosmetic profile and exit.",
     [&](const std::string&, const char*) { cosmetic_status = true; return kParseNext; }},
    // The effect checker (cosmetics_cli.cpp): no game (the log is off already: the first argv loop).
    {"--check-effect", CliArgument::Value, " <mod.dat|mod.zip|folder>  Judge one effect mod against --iso's disc and the catalog of\n"
     "    --settings-path, print one JSON line (works, works_after_restart, not_an_effect, broken) and exit:\n"
     "    0 works, 1 refused, 2 not checked.",
     [&](const std::string&, const char* value) { effect_check = value; return kParseNext; }},
    {"--check-effects", CliArgument::Value, " <folder>  Judge every effect mod in a folder the same way, a JSON line each, and exit.",
     [&](const std::string&, const char* value) { effects_check = value; return kParseNext; }},
    {"--csv", CliArgument::Value, " <out.csv>  With --check-effects, also write a CSV row per mod.",
     [&](const std::string&, const char* value) { effects_csv = value; return kParseNext; }},
    {"--pal-stock-icons", CliArgument::Flag, "  Use PAL stock icons.",
     [&](const std::string&, const char*) { gecko::option_pal_stock_icons = true; return kParseNext; }},
    {"--language", CliArgument::Value, " <game|japanese|english>  Choose the game language.",
     [&](const std::string&, const char* value) {
       const std::string text = value;
       host::g_game_language.store(text == "japanese" ? 1 : text == "english" ? 2 : 0);
       return kParseNext;
     }},
    {"--no-screen-shake", CliArgument::Flag, "  Disable screen shake.",
     [&](const std::string&, const char*) { gecko::option_no_screen_shake = true; return kParseNext; }},
    {"--gecko-codes", CliArgument::Value, " <file>  Load this file's enabled Gecko codes.",
     [&](const std::string&, const char* value) { user_gecko::set_code_patches_allowed(!gfx.native_source); user_gecko::load(value, {}, false); return kParseNext; }},
    {"--te-options", CliArgument::Value, " <hex>  Override 20XX TE's first option word.",
     [&](const std::string&, const char* value) { gx::RenderOptions::live_te_options() = (uint32_t)std::strtoul(value, nullptr, 16); return kParseNext; }},
    {"--te-options2", CliArgument::Value, " <hex>  Override 20XX TE's second option word.",
     [&](const std::string&, const char* value) { gx::RenderOptions::live_te_options2() = (uint32_t)std::strtoul(value, nullptr, 16); return kParseNext; }},
    {"--cpu-training", CliArgument::Value, " <hex>  Override the CPU training option word.",
     [&](const std::string&, const char* value) { gx::RenderOptions::live_cpu_training() = (uint32_t)std::strtoul(value, nullptr, 16); return kParseNext; }},
    {"--auto-lcancel", CliArgument::Flag, "  Automatically L-cancel.",
     [&](const std::string&, const char*) { lcancel::set_automatic(true); return kParseNext; }},
    {"--lcancel-indicator", CliArgument::Flag, "  Show the L-cancel indicator.",
     [&](const std::string&, const char*) { lcancel::set_indicator(true); return kParseNext; }},
    {"--lcancel-log", CliArgument::Value, " <file.csv>  Log the fighters' action states and L-cancel timers.",
     [&](const std::string&, const char* value) { lcancel::set_log_path(value); return kParseNext; }},
    {"--allow-matchmaking", CliArgument::Flag, "  Allow matchmaking in an automated run.",
     [&](const std::string&, const char*) { allow_matchmaking = true; return kParseNext; }, {}, true},
    {"--online-delay", CliArgument::Value, " <frames>  Set online input delay.",
     [&](const std::string&, const char* value) { slippi::online::config().delay = std::atoi(value); return kParseNext; }},
    {"--resume", CliArgument::OptionalValue, " [online]  Resume at the Online menu.",
     [&](const std::string&, const char* value) {
       if (std::string_view(value) == "online") source_port::set_resume_online();
       else host::log("resume: unknown value ignored"); // one bounded line, no untrusted argv text
       return kParseNext;
     }},
#ifndef MELEE_NO_SLIPPI
    {"--user-dir", CliArgument::Value, " <folder>  Use this Slippi user folder.",
     [&](const std::string&, const char* value) { sysgen_user_dir = value; slippi::online::config().user_dir = sysgen_user_dir; return kParseNext; }},
    {"--lobby-direct", CliArgument::Value, " <NAME#123>  Play one Slippi Direct match and exit.",
     [&](const std::string&, const char* value) {
       std::string code = value;
       const auto hash = code.find('#');
       bool valid = hash > 0 && hash < code.size() - 1 && code.size() <= 18;
       for (size_t i = 0; i < code.size(); ++i)
         if (i != hash && !((code[i] >= '0' && code[i] <= '9') || (i < hash && code[i] >= 'A' && code[i] <= 'Z'))) valid = false;
       if (!valid) { std::fprintf(stderr, "Invalid lobby Direct code\n"); return 2; }
       slippi::online::config().lobby_code = code;
       return kParseNext;
     }},
    {"--lobby-character", CliArgument::Value, " <0..255>  Choose the character for --lobby-direct.",
     [&](const std::string&, const char* value) {
       const std::string text = value; char* end = nullptr;
       long character = std::strtol(text.c_str(), &end, 10);
       if (text.empty() || *end || character < 0 || character > 255) { std::fprintf(stderr, "Invalid lobby character\n"); return 2; }
       slippi::online::config().lobby_character = (int)character;
       return kParseNext;
     }},
    {"--chat", CliArgument::Value, " <on|direct|off>  Set the quick chat policy.",
     [&](const std::string&, const char* value) { std::string v = value; slippi::online::config().chat = v == "off" ? 2 : v == "direct" ? 1 : 0; return kParseNext; }},
    {"--netplay-port", CliArgument::Value, " <port>  Force the local Slippi port.",
     [&](const std::string&, const char* value) { slippi::Matchmaking::forced_port = (uint16_t)std::atoi(value); return kParseNext; }},
    {"--local-peer", CliArgument::Value, " <idx:port:ip:port[,ip:port,...]>  Connect directly to 1-3 test peers in player-index order.",
     [&](const std::string&, const char* value) {
       // idx:local_port:ip:port[,ip:port[,ip:port]]: 2-4 instances peer directly without the
       // matchmaking server. The remotes are every other player in player-index order.
       std::string v = value; auto& lp = slippi::Matchmaking::local_peer;
       size_t a1 = v.find(':'), a2 = a1 == std::string::npos ? a1 : v.find(':', a1 + 1);
       bool ok = a1 != std::string::npos && a2 != std::string::npos;
       lp.remotes.clear();
       if (ok) {
         lp.local_index = std::atoi(v.substr(0, a1).c_str()); lp.local_port = (uint16_t)std::atoi(v.substr(a1 + 1, a2 - a1 - 1).c_str());
         for (size_t at = a2 + 1; ok && at <= v.size();) {
           size_t comma = v.find(',', at);
           std::string r = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
           size_t c = r.find(':');
           ok = c != std::string::npos && c > 0 && std::atoi(r.substr(c + 1).c_str()) > 0;
           lp.remotes.push_back(r);
           if (comma == std::string::npos) break;
           at = comma + 1;
         }
       }
       ok = ok && lp.local_port > 0 && !lp.remotes.empty() && lp.remotes.size() <= 3 && lp.local_index >= 0 && lp.local_index <= (int)lp.remotes.size();
       if (!ok) { std::fprintf(stderr, "--local-peer idx:port:ip:port[,ip:port[,ip:port]] (2-4 players, idx < players)\n"); return 2; }
       lp.enabled = true;
       return kParseNext;
     }, {}, true},
    {"--test-stage", CliArgument::Value, " <id>  Force the stage for a --local-peer test.",
     [&](const std::string&, const char* value) {
       auto& lp = slippi::Matchmaking::local_peer;
       const int stage = std::atoi(value);
       if (stage < 0 || stage > 0xFFFF) { std::fprintf(stderr, "--test-stage requires a legal Slippi stage id\n"); return 2; }
       lp.test_stage = stage;
       return kParseNext;
     }, {}, true},
    {"--record-native", CliArgument::Flag, "  Enable native replay recording.",
     [&](const std::string&, const char*) { _putenv_s("MELEE_SOURCE_RECORD", "1"); return kParseNext; }},
    {"--slippi-menus", CliArgument::Value, " <on|off>  Select Slippi or retail menus.",
     [&](const std::string&, const char* value) {
       const std::string v = value;
       if (v != "on" && v != "off") { std::fprintf(stderr, "--slippi-menus on|off\n"); return 2; }
       source_port::set_slippi_menus(v == "on");
       return kParseNext;
     }},
    {"--online-test", CliArgument::Value, " <mode[:character[:color]]>  Start a test match with --local-peer.",
     [&](const std::string&, const char* value) {
       online_test_arg = true;
       if (!source_port::set_online_test(value)) {
         std::fprintf(stderr, "--online-test direct|unranked|ranked|teams[:<character>[:<color>]]"
#if MU_SQUAD
         // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
         "|squad:<ck>/<color>[,...]"
#endif
         " (with --local-peer)\n");
         return 2;
       }
       return kParseNext;
     }, {}, true},
#if MU_SQUAD
    {"--squad-test-invalid-lineup", CliArgument::Flag, "  Use an invalid lineup in a Squad Strike test match.",
     [&](const std::string&, const char*) { squad_test_invalid_lineup = true; return kParseNext; }, {}, true},
#endif
#endif
    {"--p2p-port", CliArgument::Value, " <local UDP port>  Set the local port for a peer-to-peer match.", apply_p2p},
    {"--p2p-peer", CliArgument::Value, " <ip:port>  Add a peer address. Repeat to add candidates.", apply_p2p},
    {"--p2p-slot", CliArgument::Value, " <0|1>  Select the local player slot.", apply_p2p},
    {"--p2p-chars", CliArgument::Value, " <id[/color]:id[/color]>  Choose fighters for slots 0 and 1.", apply_p2p},
    {"--p2p-stage", CliArgument::Value, " <stage id>  Choose the stage.", apply_p2p},
    {"--p2p-seed", CliArgument::Value, " <hex>  Set the shared RNG seed.", apply_p2p},
    {"--p2p-delay", CliArgument::Value, " <1..15>  Set input delay in frames.", apply_p2p},
    {"--p2p-identity", CliArgument::Value, " <file>  Use this local identity.", apply_p2p},
    {"--p2p-expect", CliArgument::Value, " <64 hex digits>  Require this peer identity key.", apply_p2p},
    {"--p2p-result", CliArgument::Value, " <file>  Write the session result.", apply_p2p},
    {"--p2p-names", CliArgument::Value, " <name:name>  Set the names for slots 0 and 1.", apply_p2p},
    {"--p2p-connect-seconds", CliArgument::Value, " <1..600>  Set the connection timeout.", apply_p2p},
    {"--p2p-games", CliArgument::Value, " <count>  Set the number of games (0 means no limit).", apply_p2p},
    {"--script", CliArgument::Value, " <file>  Read scripted controller input.",
     [&](const std::string&, const char* value) {
       if (!host::input_load_script(value)) {
         std::fprintf(stderr, "cannot load input script\n");
         return 1;
       }
       scripted = true;
       return kParseNext;
     }, [](const char*) { host::test_env::automated = true; }},
    {"--match", CliArgument::Value, " <stage>:<p1>[:<p2>...]  Force match selections. A player takes kind[/c<level>][/x<costume>].",
     [&](const std::string&, const char* value) {
       if (!source_port::set_match(value)) {
         std::fprintf(stderr, "--match <stage>:<p1>[:<p2>...], each player <kind>[/c<level>][/x<costume>]\n");
         return 2;
       }
       return kParseNext;
     }},
    {"--time-base", CliArgument::Value, " <ticks>  Set a fixed time base for tests.",
     [&](const std::string&, const char* value) { o.time_base = std::strtoull(value, nullptr, 0); return kParseNext; }},
    {"--rng-seed", CliArgument::Value, " <seed>  Set the RNG seed at the first VS frame.",
     [&](const std::string&, const char* value) { o.rng_seed = (uint32_t)std::strtoul(value, nullptr, 0); o.rng_seed_set = true; return kParseNext; }},
    {"--state-trace", CliArgument::Value, " <file>  Write a simulation state trace.",
     [&](const std::string&, const char* value) { o.state_trace = value; return kParseNext; }},
    {"--state-digest", CliArgument::Value, " <file>  Write a per-frame state digest.",
     [&](const std::string&, const char* value) { o.state_digest = value; return kParseNext; }},
    {"--input-log", CliArgument::Value, " <file>  Log controller input.",
     [&](const std::string&, const char* value) { o.input_log = value; return kParseNext; }},
    {"--trace-func", CliArgument::Value, " <address[:limit]>  Trace a guest call by hex address.",
     [&](const std::string&, const char* value) {
       const char* spec = value; uint32_t addr = (uint32_t)std::strtoul(spec, nullptr, 16); uint32_t limit = 40;
       if (const char* colon = std::strchr(spec, ':')) limit = (uint32_t)std::strtoul(colon + 1, nullptr, 10);
       if (!addr) { std::string name(spec, std::strchr(spec, ':') ? std::strchr(spec, ':') - spec : std::strlen(spec));
         for (size_t i = 0; i < guest::name_table_count; ++i) if (name == guest::name_table[i].name) { addr = guest::name_table[i].addr; break; } }
       if (!addr) { std::fprintf(stderr, "unknown function %s\n", spec); return 2; }
       ppc::add_trace_func(addr, limit);
       return kParseNext;
     }},
    {"--trace-calls", CliArgument::Flag, "  Trace calls into the host.",
     [&](const std::string&, const char*) { o.trace_calls = true; return kParseNext; }},
    {"--quiet", CliArgument::Flag, "  Reduce log output.",
     [&](const std::string&, const char*) { o.quiet = true; return kParseNext; }},
    {"--log-file", CliArgument::ValueBeforeSettings, " <file>  Write logs to this path.",
     [&](const std::string&, const char* value) { o.log_file = value; return kParseNext; },
     // Loading settings can open the log. Set its path before the first log line.
     [&](const char* value) { o.log_file = value; }},
    {"--hang-watch", CliArgument::Value, " <seconds>  Set the hang watchdog timeout.",
     [&](const std::string&, const char* value) { o.hang_watch = std::atof(value); return kParseNext; }},
    {"--profile", CliArgument::Flag, "  Profile simulation.",
     [&](const std::string&, const char*) { sim_profiler::request(false); return kParseNext; }},
    {"--profile-render", CliArgument::Flag, "  Profile simulation and rendering.",
     [&](const std::string&, const char*) { sim_profiler::request(true); return kParseNext; }},
    {"--lag-test", CliArgument::Flag, "  Enable the adapter lag test.", accept,
     [](const char*) { _putenv_s("MELEE_LAG_TEST", "1"); }},
  };
  const auto usage = [&] { print_usage(options); };
  const auto find_option = [&](std::string_view name) {
    return std::find_if(std::begin(options), std::end(options),
                        [&](const CliOption& entry) { return name == entry.name; });
  };

  // The unattended options again, for every platform (Windows read them already from the initialiser
  // slot; this is the same answer, and covers a build where that slot did not run). --print-info ends
  // the process inside answer().
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print-info") == 0) print_info::answer();
    if (means_no_dialogs(argv[i])) turn_dialogs_off();
    // The effect checker's JSON lines are stdout's only text: the log, which also goes there, is off
    // from before its first line.
    if (std::strcmp(argv[i], "--check-effect") == 0 || std::strcmp(argv[i], "--check-effects") == 0)
      host::options.quiet = true;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
    usage();
    return 0;
  }
  // --prewarm-only must run from the same working directory as a normal launch.
  if (app::prewarm_only::requested(argc, argv)) {
    const int result = run_prewarm_only(argc, argv, usage);
    host::log_flush();
    host::end_process(result);   // Windows too: skip CRT/DLL unload inside graphics drivers
  }
  // The compatibility build (an SSE2 baseline) exists for exactly these processors: only an AVX2
  // build has anything to refuse.
  if (std::strcmp(MELEE_CPU_BASELINE_NAME, "AVX2") == 0 && !cpu_has_avx2()) {
    const char* msg = PRODUCT_NAME " needs a CPU with AVX2 (Intel Haswell 2013 or newer, AMD Ryzen or newer). This CPU does not support it.";
    std::fprintf(stderr, "%s\n", msg);
    if (!host::g_no_dialogs) MessageBoxA(nullptr, msg, product::kName, MB_ICONERROR | MB_OK);
    return 3;
  }
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--version") { std::printf("%s\n", MELEE_PORT_VERSION); return 0; }
#ifdef _WIN32
  // Fighter plugin approval from a launcher or a script (plugin_gate.h): before anything logs, so a
  // listing never touches a running game's log, and before any thread starts.
  if (plugin_gate::cli_requested(argc, argv)) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string answer;
    const int code = plugin_gate::cli_run(argc, argv, std::filesystem::path(exe).parent_path(), &answer);
    print_info::write_out(answer.data(), answer.size());
    return code;
  }
#endif

  if (!source_port::reserve_memory()) return 1;
  TimerResolution timer_resolution;
  // This thread runs the game (and, without --threaded-renderer, draws it). At normal priority any
  // busy program on the PC (a compile, a browser tab, an encoder) takes turns with it, and one lost
  // turn is a late 60 Hz tick: replays on a loaded PC went from ~400 ticks over 16.7 ms to under 5
  // with the game above normal. The tick sleeps most of each frame, so the rest of the PC keeps
  // the remaining time; the render thread and the audio thread are raised where they start.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  // Windows power throttling (EcoQoS) off for this process: on CPUs with performance and efficiency
  // cores, Windows can move a throttled process onto the efficiency cores, and the game then runs
  // slow for as long as it stays there. The game sleeps most of each frame, so this costs no power
  // while it waits. Ignored by Windows versions without the setting.
#ifdef _WIN32
  DWORD timer_resolution_error = 0, timer_fallback_error = 0;
#endif
  {
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // Also: always honor this process's timer resolution request (Windows 11 otherwise drops it
    // whenever it judges the window hidden or covered, and the frame waits go coarse).
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4 /* PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION */;
    throttling.StateMask = 0;   // execution speed never throttled, timer resolution never ignored
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling)) {
#ifdef _WIN32
      timer_resolution_error = GetLastError();
#endif
      // Windows 10 does not know the timer bit and refuses the whole request: ask for the speed part alone.
      throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef _WIN32
      if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling))
        timer_fallback_error = GetLastError();
#else
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);
#endif
    }
  }
  // Only options that affect settings or startup run in this pass. As before, only the
  // early value options skip their following token here. The full pass validates all arguments.
  for (int i = 1; i < argc; ++i) {
    const auto option = find_option(argv[i]);
    if (option == std::end(options) || !option->prepare) continue;
    if (option->argument == CliArgument::ValueBeforeSettings) {
      if (i + 1 < argc) option->prepare(argv[++i]);
    } else {
      option->prepare(nullptr);
    }
  }
  if (host::gameplay::vanilla_only()) {
    // A previously used diagnostic environment must not turn a vanilla listing into a lag test.
    _putenv_s("MELEE_LAG_TEST", "");
    host::log("gameplay: vanilla-only session; saved gameplay mods are suspended");
  }
  // A launcher can keep a settings-window process on standby beside a running game. Sharing
  // melee_port.log let it rotate the game's log away at its start, so a player's log held only the
  // panel's three lines and every desync line of the match was lost.
  if (settings_window_only && host::options.log_file.empty()) host::options.log_file = "melee_settings.log";
#ifdef _WIN32
  // Log only after the early --log-file parse, so the report uses the game's chosen run log.
  host::log("timing: timeBeginPeriod(1) %s", timer_resolution.raised ? "ok" : "failed");
  const bool timer_unsupported = timer_resolution_error == ERROR_INVALID_PARAMETER ||
      timer_resolution_error == ERROR_NOT_SUPPORTED || timer_resolution_error == ERROR_CALL_NOT_IMPLEMENTED ||
      timer_resolution_error == ERROR_INVALID_FUNCTION;
  if (timer_resolution_error)
    host::log("timing: timer resolution never ignored %s (%lu); execution-speed fallback %s (%lu)",
              timer_unsupported ? "unsupported" : "denied", (unsigned long)timer_resolution_error,
              timer_fallback_error ? "failed" : "ok", (unsigned long)timer_fallback_error);
  else host::log("timing: timer resolution never ignored ok (0)");
  HANDLE diagnostic_token = nullptr;
  DWORD is_appcontainer = 0, token_bytes = 0;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &diagnostic_token)) {
    const BOOL readable = GetTokenInformation(diagnostic_token, TokenIsAppContainer,
        &is_appcontainer, sizeof is_appcontainer, &token_bytes);
    const DWORD token_error = readable ? ERROR_SUCCESS : GetLastError();
    CloseHandle(diagnostic_token);
    if (readable) host::log("timing: sandbox appcontainer %s", is_appcontainer ? "yes" : "no");
    else host::log("timing: sandbox appcontainer unknown (%lu)", (unsigned long)token_error);
  } else host::log("timing: sandbox appcontainer unknown (%lu)", (unsigned long)GetLastError());
#endif

  gfx.pc_settings = !automated;
  crash_set_dialog(!automated && !no_crash_dialog && !host::g_no_dialogs);
  if (no_crash_dialog) host::log("dialogs: none in this run; a refusal, fatal error or crash is logged and reported in a file (--no-dialogs)");
  o.no_gc_adapter = automated;   // a hidden test run must not take the adapter from a game the player is running
  if (std::getenv("MELEE_NO_GC_ADAPTER")) o.no_gc_adapter = true;   // same, for a visible test window
#ifdef _WIN32
  SetUnhandledExceptionFilter(crash_filter);
  AddVectoredExceptionHandler(1, note_throw);
#else
  linux_install_crash_handlers();
#endif
  host::set_die_hook(die_report);
  // Automated runs: 60 Hz with sub-frame animation off (the defaults in gx/render_options.h are the
  // monitor's rate with Predict ahead, for every launch a person makes). An explicit --fps or
  // --frame-mode below still wins over either.
  if (automated) { gfx.fps_cap = 60; if (!explicit_frame_mode) gfx.subframe = gx::SubFrameMode::Off; }
  if (!automated) {
    // A person launching the game wants to hear it. The zero default is there for automated runs,
    // which never reach this branch, and it used to be hidden by the launcher passing --volume 70 on
    // every start; that override was removed because it also overwrote the player's saved settings,
    // which left anyone without a saved volume silent. Set before the file is read, so a saved
    // volume still wins, and an explicit --volume below wins over both.
    o.volume = 70;
    gx::load_pc_settings(gfx, o.volume);
    source_port::mods::set_auto_detect(true);
    // Opt-in, and only ever from a saved setting: an automated or headless run never gets here, so
    // it can never publish. With the setting off no thread is started and no pipe is opened.
    // Only in a build with its own Discord application (product_identity.h kDiscordAppId).
    if (gfx.discord_presence && host::discord::available()) host::discord::enable(true);
    threaded = true;
  } else if (load_settings) {
    // Test runs of the settings themselves: read --settings-path the way a
    // normal launch does, before the flags below, so an explicit flag still wins. Discord is never
    // started from an automated run.
    gx::load_pc_settings(gfx, o.volume);
  }
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (host::gameplay::vanilla_only() && host::gameplay::conflicts_with_vanilla(a)) {
      std::fprintf(stderr, "--vanilla-only cannot be combined with %s; launch the mod listing without --vanilla-only\n", a.c_str());
      return 2;
    }

    // Only the two optional-equals options accept an attached value. Required values are
    // separate tokens, even when the following token begins with a dash.
    const size_t equals = a.find('=');
    const std::string_view name(a.data(), equals == std::string::npos ? a.size() : equals);
    auto option = find_option(name);
    if (option != std::end(options) && equals != std::string::npos && option->argument != CliArgument::OptionalEquals)
      option = std::end(options);
    // An unknown --p2p-* name still reaches the peer parser for its specific diagnostic.
    const bool p2p = option == std::end(options) && a.rfind("--p2p-", 0) == 0;
    if (!p2p && (option == std::end(options) || !option->apply)) {
      usage();
      return 2;
    }
    // A public build takes no test fixture's entry point (public_build.h): refused before its value is read.
    if (!p2p && option->test_only && host::public_build::kOn) {
      std::fprintf(stderr, "%s is a test option; this build of %s does not take it\n", option->name, product::kName);
      return 2;
    }
    const CliArgument kind = p2p ? CliArgument::Value : option->argument;
    const char* value = nullptr;
    switch (kind) {
      case CliArgument::Flag:
        break;
      case CliArgument::ValueBeforeSettings:
      case CliArgument::Value:
        // Avoid CRT teardown while startup threads (such as Discord's) can still be running.
        if (i + 1 >= argc) { usage(); host::end_process(2); }
        value = argv[++i];
        break;
      case CliArgument::OptionalValue:
        value = i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "";
        break;
      case CliArgument::OptionalEquals:
        if (equals != std::string::npos) value = a.c_str() + equals + 1;
        break;
    }
    const int result = p2p ? apply_p2p(a, value) : option->apply(a, value);
    if (result == kParseUsage) { usage(); return 2; }
    if (result != kParseNext) return result;
  }
#ifdef MELEE_NO_SLIPPI
  // Retail menus and the game's own music stream; the code set and the training packs follow the
  // same switches as in the normal build (--vanilla-game turns them off).
  (void)allow_matchmaking;
  if (!source_p2p::check()) { usage(); return 2; }
#else
  if (!source_p2p::check()) { usage(); return 2; }
  // One session per run. A peer-to-peer match and a Slippi match would both answer the game's
  // session commands, and a replay has no live session at all: refuse the mix before anything starts.
  if (source_p2p::requested()) {
    const char* other = !slippi::online::config().lobby_code.empty() ? "--lobby-direct"
                        : online_test_arg ? "--online-test" : !replay_arg.empty() ? "--replay" : nullptr;
    if (other) {
      std::fprintf(stderr, "--p2p-* starts a peer-to-peer match and cannot be combined with %s\n", other);
      return 2;
    }
  }
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  if (!source_port::check_squad_test(slippi::Matchmaking::local_peer.enabled, squad_test_invalid_lineup, false)) {
    std::fprintf(stderr, "--online-test squad:<ck>/<color>[,...] requires --local-peer; --squad-test-invalid-lineup and --squad-test-net-delay require squad mode\n");
    return 2;
  }
#endif
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#endif
  if ((hidden || headless || scripted) && !allow_matchmaking) slippi::Matchmaking::server_allowed = false;
  if (slippi::Matchmaking::local_peer.test_stage >= 0) {
    const int stage = slippi::Matchmaking::local_peer.test_stage;
    // A mod disc (--mod-base-iso) has its own stages: any id, so a mod's stage can be tested.
    // Peach's Castle (4) and Kongo Jungle (5) too: Direct and Teams players can pick them, and Slippi
    // 3.7.0 resets a state of each at stage init.
    const bool legal = stage == 0x2 || stage == 0x3 || stage == 0x8 || stage == 0x1C || stage == 0x1F || stage == 0x20 ||
                       stage == 0x4 || stage == 0x5 || !o.mod_base_iso.empty();
    if (!slippi::Matchmaking::local_peer.enabled || !legal) {
      std::fprintf(stderr, "--test-stage requires --local-peer and a legal tournament stage id (2, 3, 8, 28, 31, or 32; any on a mod disc)\n");
      return 2;
    }
  }
#endif
  // The game reads this flag (MU_GAME_OPTION_WIDESCREEN) and runs its native version of Slippi's code.
  gecko::option_widescreen = gfx.widescreen;
#ifdef MELEE_NO_SLIPPI
  if (gfx.widescreen) std::fprintf(stderr, "widescreen: 16:9 code on\n");
#else
  if (gfx.widescreen) std::fprintf(stderr, "widescreen: Slippi 16:9 on\n");
#endif
  gecko::option_lagless_fod = !gfx.fod_reflections; // the Gecko flag is the inverse of the UI label
  if (fps_requested && gfx.subframe == gx::SubFrameMode::Off) {
    std::fprintf(stderr, "--fps requires explicit experimental --frame-mode interpolate, extrapolate or authored\n");
    return 2;
  }
  // Cosmetic catalog/profile storage follows --settings-path just like controller and video
  // settings. Configure it for normal, standalone-settings, and automated diagnostic launches.
  host::cosmetics::configure(gfx.settings_path);
  host::menu_mods::configure(gfx.settings_path);     // menu mods from the Mods menu: <settings dir>/MenuMods
  host::new_stages::configure(gfx.settings_path);    // new stages from the Mods menu: <settings dir>/NewStages
  host::skins::configure_cache(gfx.settings_path);   // opponents' skins: <settings dir>/OpponentSkins
  // On unless the settings file says "opponentskins 0"; said once, since only a change is logged otherwise.
  host::log("opponent skins: %s at start", host::skins::enabled() ? "on" : "off");
#ifdef _WIN32
  // Fighter plugin approvals sit beside the settings file too (plugin_gate.h).
  plugin_gate::configure_run(gfx.settings_path, [](const char* line) { host::log("%s", line); });
#endif

  host::cosmetics::set_tool_recovery(true);
  // Tests: MELEE_BUILD_LOOKS=<patch folder> builds the exact looks' packs from --iso and exits (0 ready, 1 not).
  if (const char* looks = host::test_env::getenv("MELEE_BUILD_LOOKS"); looks && *looks) return cosmetics_cli::build_looks(looks, o);
  if (!effect_check.empty()) return cosmetics_cli::check_effect(effect_check, o);
  if (!effects_check.empty()) return cosmetics_cli::check_effects(effects_check, effects_csv, o);
  if (!cosmetic_import.empty() || !cosmetic_scan_disc.empty() || !cosmetic_select.empty() ||
      cosmetic_deselect_requested || cosmetic_enable_effects || cosmetic_restore || cosmetic_status)
    return cosmetics_cli::run(cosmetic_import, cosmetic_scan_disc, cosmetic_select, cosmetic_deselect,
                              cosmetic_deselect_requested, cosmetic_enable_effects, cosmetic_restore, cosmetic_status, o);

  // The settings window, with no game behind it: the one place settings are changed (the game
  // window has no settings overlay). A launcher opens it instead of booting the whole game. Before the disc check, because it needs no disc.
  if (settings_window_only) {
    gfx.pc_settings = true;
    gx::load_pc_settings(gfx, o.volume);
    // Shown again from standby: the settings file and the skin profile as the game or an earlier panel
    // left them. The profile was read once at the panel's start, so a skin set back to standard in the
    // game showed as still on here, and any skin change in this panel saved those old picks back.
    const int rc = app::run_settings_window(gfx, settings_standby, [&] {
      gx::load_pc_settings(gfx, o.volume); gfx.pc_settings = true;
      host::cosmetics::configure(gfx.settings_path);
      host::log("settings: reloaded for show, cosmetic profile %s",
                host::cosmetics::profile_enabled() ? "enabled" : "disabled");
    });
    // The panel polls every controller so its live readouts work, which starts the adapter and
    // Switch Pro threads, and it can start an optional download. Returning straight from here left those
    // threads running into static destruction, where a joinable std::thread ends the process: every
    // close of this window was a crash (0xC0000409), and with Windows Error Reporting collecting it,
    // the window took a long time to go away. Same shutdown as the game's, below.
    host::downloads::shutdown();
    host::discord::shutdown();
    host::gcadapter_shutdown();
    host::switchpro_shutdown();
    slippi::shutdown();
    return rc;
  }
  if (!card_self_test_dir.empty()) return source_port::card_self_test(card_self_test_dir.c_str()) ? 0 : 1;
  if (o.iso.empty()) { usage(); return 2; }
  if (!host::disc_open(o.iso)) { std::fprintf(stderr, "cannot open ISO %s\n", o.iso.c_str()); return 1; }
  // Set the window title before asset preparation starts.
  {
    const char* version = MELEE_PORT_VERSION;
    host::window_title_base() = product::kWindowTitleW;   // product_identity.h
    std::wstring base = std::wstring(product::kWindowTitleW) + L" ";
    for (const char* c = version; *c; ++c) base += (wchar_t)(unsigned char)*c;
    base += L"  |  Source Port";
    // Players see just the name; the version, engine and rates stay behind the Performance overlay.
    host::window_title_debug() = base;
  }
  std::unique_ptr<gx::Backend> backend;
  bool window_made = false;
  auto make_window = [&] {
    window_made = true;
    // Vulkan chosen but not usable here: choose the fallback before creating the SDL window. For both paths
    // (upstream v0.9.86): the threaded renderer (every normal launch) makes its window too, and without this a
    // Linux Vulkan choice got an OpenGL window whose Vulkan surface could not be created, so the game stopped.
    if (!headless) {
      prepare_render_backend(gfx);
    }
    if (!headless && threaded) {
      backend = gx::create_threaded_backend(gfx, !hidden);
    } else if (!headless) {
#ifdef __APPLE__
      const bool defer_fullscreen = gfx.fullscreen && !hidden;
      if (defer_fullscreen) host::window_prefer_simple_fullscreen();
      void* hwnd = host::window_create(gfx.window_w, gfx.window_h, host::window_title_base().c_str(), !hidden && !defer_fullscreen);
      if (defer_fullscreen) host::window_defer_fullscreen_show();
#else
      void* hwnd = host::window_create(gfx.window_w, gfx.window_h, host::window_title_base().c_str(), !hidden
#ifdef _WIN32
          , true
#endif
      );
      if (gfx.fullscreen && !gfx.exclusive_fullscreen) host::window_set_fullscreen(true);
#endif
      int client_w = gfx.window_w, client_h = gfx.window_h;
      // As on the render thread (threaded_backend.cpp): the window keeps answering while the renderer starts.
      host::window_responsive([&] { backend.reset(gx::create_render_backend(hwnd, client_w, client_h, gfx)); });
      host::window_set_resize_callback([renderer = backend.get()](int w, int h) { gx::render_resize(renderer, w, h); });
      host::g_has_window = true;
    }
    if (backend) host::log("startup: window and renderer ready%s", host::startup_mark().c_str());
  };
#ifdef _WIN32
  if (threaded && !headless) make_window();
#endif
#ifndef MELEE_NO_SLIPPI
  // A bare run without --sys-dir gets the compiled default, a source-tree path relative to the working
  // folder. A packaged build carries its Sys beside the exe (the app and a launcher usually pass it),
  // so use that when the default is not there instead of failing Sys generation at boot.
  if (o.sys_dir == "port/slippi_sys" && !std::filesystem::exists(std::filesystem::u8path(o.sys_dir))) {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
      const auto beside = std::filesystem::path(self).parent_path() / L"Sys";
      if (std::filesystem::is_directory(beside)) {
        o.sys_dir = beside.u8string();
        host::log("sys: no --sys-dir given; using %s beside the exe", o.sys_dir.c_str());
      }
    }
  }
#endif
  // No --user-dir (say, --iso alone), or one inside the game folder: prepare_sys_assets generates the two
  // managed Sys files into the player's own data folder (sysgen::user_data_dir), on every system.
  if (const std::string disc_problem = host::prepare_sys_assets(sysgen_user_dir, sysgen_data_dir); !disc_problem.empty()) {
    // The player's disc, not a crash: say so and leave without a crash report (as --vanilla-only does).
    std::fprintf(stderr, "%s\n", disc_problem.c_str());
    host::log_flush();
#ifdef __APPLE__
    if (!automated) {
      std::error_code ec;
      std::filesystem::remove(std::filesystem::path(sysgen_user_dir) / "disc-path", ec);
      if (!host::g_no_dialogs) mac_host::show_launch_error(disc_problem);
    }
#endif
#ifdef _WIN32
    if (!automated && !host::g_no_dialogs) MessageBoxA(nullptr, disc_problem.c_str(), product::kName, MB_ICONINFORMATION | MB_OK);
    ExitProcess(2);
#else
    std::_Exit(2);
#endif
  }
  // The profile picked in the settings panel, unless the command line gave its own mod layers. A
  // profile that no longer loads starts the retail game rather than refusing to start.
  if (gx::RenderOptions::mod_features_available() && !source_port::has_mod_layers() && !gfx.mod_profile.empty()) {
    if (!source_port::set_mod_profile(gfx.mod_profile.c_str(), false))
      host::log("mods: saved profile %s could not be loaded; starting the retail game", gfx.mod_profile.c_str());
  }
#ifdef MELEE_NO_SLIPPI
  if (!replay_arg.empty()) {
    std::fprintf(stderr, "--replay is not available in this build\n");
    return 2;
  }
#else
  if (!replay_arg.empty() && !source_port::set_replay(replay_arg.c_str())) {
    std::fprintf(stderr, "cannot play replay %s\n", replay_arg.c_str());
    return 1;
  }
#endif
  // An exact look (Diet Classic, Diet 64, Crystal) serves its pack from this start when the player built it;
  // otherwise its renderer version stands in. The built discs are scanned now that the game disc is open.
  {
    host::cosmetics::scan_look_packs();
    const std::string look = gx::exact_look_of(gfx);
    std::string active;
    if (!look.empty())
      for (const auto& pack : host::cosmetics::look_packs())
        if (pack.name == look && pack.built) active = look;
    // --look-iso: a mod disc's looks over the standard game, for this start only, in place of an exact
    // look. Its costumes, stages, HUD and effects are served by the rules of an exact look (online, only
    // what passes the look check), and its alternate stages are this session's stages. Nothing is
    // saved; none of its code runs.
    std::string mod_look;
    if (!o.look_iso.empty() && o.mod_base_iso.empty()) {
      std::string name = host::cosmetics::disc_pack_name(o.look_iso);
      if (name.empty()) name = std::filesystem::u8path(o.look_iso).stem().u8string() + " disc";
      std::string error;
      const auto scan = host::cosmetics::scan_disc_skins(o.look_iso, name, &error);
      mod_look = host::cosmetics::disc_pack_name(o.look_iso);
      if (mod_look.empty())
        host::log("looks: %s has no looks this game can use (%s); the standard game plays", o.look_iso.c_str(),
                  (error.empty() ? scan.message : error).c_str());
      else {
        host::cosmetics::set_session_stage_pack(o.look_iso);
        host::log("looks: the standard game with the looks of %s", mod_look.c_str());
      }
    }
    const bool exact = !active.empty() && mod_look.empty();
    if (!mod_look.empty()) active = mod_look;
    host::cosmetics::set_active_look_pack(active);
    gx::look_live().exact.store(exact, std::memory_order_relaxed);
    gx::look_live().exact_low_poly.store(!exact || active != "Crystal", std::memory_order_relaxed);   // Crystal's fighters are the full models
    if (exact) host::log("looks: %s from the player's pack", active.c_str());
  }
  if (host::gameplay::vanilla_only()) {
    source_port::mods::ScanOptions check;
    std::string verification;
    const auto disc = source_port::mods::identify_vanilla(std::filesystem::u8path(o.iso), check, &verification);
    if (!verification.empty()) host::log("gameplay: %s%s", verification.c_str(), host::startup_mark().c_str());
    if (disc.kind != source_port::mods::Kind::Vanilla) {
      host::log("--vanilla-only requires an unmodified Melee NTSC 1.02 ISO. %s", disc.message.c_str());
      std::fprintf(stderr, "--vanilla-only requires an unmodified Melee NTSC 1.02 ISO. %s\n", disc.message.c_str());
      host::log_flush();
      // Tell the player and leave without a crash report: by now background work has started, and
      // returning from main ended in abort ("the game stopped itself") in place of this message.
      if (!automated && !host::g_no_dialogs)
        MessageBoxA(nullptr, ("This disc image cannot be used for the standard game. " + disc.message).c_str(),
                    product::kName, MB_ICONINFORMATION | MB_OK);
      ExitProcess(2);
    }
    // A card can itself carry executable mods (20XX TE). Keep this listing's saves separate,
    // without moving, copying or clearing any of the player's ordinary or modded card files.
    o.card_dir = (std::filesystem::u8path(o.card_dir) / "VanillaOnly").u8string();
    explicit_card_dir = true;
    host::log("gameplay: verified retail disc; vanilla-only memory card %s%s", o.card_dir.c_str(), host::startup_mark().c_str());
  }
#ifdef __APPLE__
  if (!automated && !scripted && mac_host::melee_disc_header(o.iso))
    mac_host::remember_disc(std::filesystem::path(sysgen_user_dir), std::filesystem::u8path(o.iso));
#endif
  // Controllers do not count as activity to Windows, so a session played only on a pad let the
  // display power off after the idle timeout (monitors going black mid-game until the mouse moved).
  // Held by this thread for as long as the game runs; Windows drops it when the process exits.
  if (!hidden) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

#ifdef __APPLE__
  // Accept --threaded-renderer from the desktop launch spec. AppKit requires the main thread.
  threaded = false;
  if (automated || scripted) {
    gfx.slippi_spectator = false;
    gx::RenderOptions::live_slippi_spectator() = false;
  }
#endif
  // Made here unless it was made right after the disc opened (above).
  if (!window_made) make_window();
  // Texture packs: scan before the game starts so the settings list is right, and decode up front
  // when the player asked for that, with the wait shown in the title bar rather than as a silent
  // half minute. Decoding runs on its own thread, so the game keeps booting while it works.
  gx::texpack::configure(gfx.custom_textures, gfx.dump_textures);
  gx::texpack::refresh_packs();
  if (gfx.custom_textures && gfx.prefetch_textures) {
    gx::texpack::prefetch_begin();
    while (gx::texpack::prefetching()) {
      uint64_t done = 0, total = 0;
      gx::texpack::prefetch_progress(&done, &total);
      wchar_t title[192];
      swprintf_s(title, L"%ls  |  loading textures %llu / %llu", host::window_title_base().c_str(),
                 (unsigned long long)done, (unsigned long long)total);
      host::window_set_title(title);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    host::window_set_title(host::window_title_base().c_str());
  }
  gx::set_authored_capture(gfx.subframe == gx::SubFrameMode::Authored || gfx.subframe == gx::SubFrameMode::AuthoredInterpolate);
  host::disc_prefetch_wait();
  gx::init(backend.get());
  host::audio_set_buffering(gfx.audio_mode, gfx.audio_buffer_ms);
  host::audio_set_device(gfx.audio_device.c_str());
  host::audio_set_asio(gfx.audio_asio_driver.c_str(), gfx.audio_asio_buffer);
  // Hidden runs are test automation (players never start hidden): they use the player's Auto memory
  // but never save to it, so a loaded test machine cannot raise the player's buffer.
  host::audio_set_remember(!hidden);
  host::audio_open(o.volume, o.audio_dump.c_str(), !headless);
  arm_test_crash(automated);   // no-op unless MELEE_TEST_CRASH is set on a hidden run
  // The settings that decide how a match feels, in one line, so a report on stutter or lag says them.
  {
    const char* saved_backend = gfx.api == gx::RenderApi::D3D11 ? "D3D11" : gfx.api == gx::RenderApi::Vulkan ? "Vulkan" : "D3D12";
    const auto backend_label = app::settings_backend_label(saved_backend,
        backend ? gx::started_render_backend() : "none (headless)");
    static const char* const subframes[] = {"off", "extrapolate", "interpolate", "authored", "authored + interpolate"};
    host::log("settings: %s, frame cap %s, sub-frame %s, internal resolution %s, upscaler %d, vsync %s, %s%s",
              backend_label.c_str(),
              gfx.fps_cap < 0 ? "monitor" : gfx.fps_cap == 0 ? "off" : std::to_string((int)gfx.fps_cap).c_str(),
              subframes[std::min<size_t>((size_t)gfx.subframe, 4)],
              gfx.efb_scale ? ("x" + std::to_string(gfx.efb_scale)).c_str() : "auto", gfx.dlss_mode,
              gfx.vsync ? "on" : "off", gfx.exclusive_fullscreen ? "exclusive fullscreen" : "windowed or borderless",
              host::startup_mark().c_str());
    // The picture's colour and filtering settings, so a report about colours (11-17: menu outlines
    // looking white instead of gold) says what was set.
    host::log("settings: picture brightness %.2f, contrast %.2f, vibrance %.2f, sharpening %d%%, MSAA %d, SSAA %d, "
              "anisotropy %d, HDR output %s, shader preset %s, CRT filter %s",
              gfx.brightness, gfx.contrast, gfx.vibrance, (int)(gfx.sharpness * 100.0f + 0.5f), gfx.msaa, gfx.ssaa,
              gfx.anisotropy, gfx.hdr_output ? "on" : "off", gfx.shader_preset.empty() ? "none" : gfx.shader_preset.c_str(),
              gfx.crt_filter ? "on" : "off");
  }

  // The Lossless adapter's lag test (Settings > Advanced, or --lag-test): the native game and the
  // settings load read MELEE_LAG_TEST (shim/mu_lagtest.c, host/lag_test_codes.h). Its app runs Dolphin
  // with no memory card, so the default rules the codes set apply: slot A answers "no card" in this
  // launch (source_host_card.cpp), and its folder points at an empty one of its own so
  // nothing in the session can touch the player's saves.
  if (!host::gameplay::vanilla_only() && (gfx.lossless_lag_test || std::getenv("MELEE_LAG_TEST"))) {
    // The saved switch arms this launch only: the next one is an ordinary game again.
    if (gfx.lossless_lag_test && !gx::consume_lag_test_launch(gfx.settings_path))
      host::log("lag test: could not clear the saved switch in %s", gfx.settings_path.c_str());
    gfx.lossless_lag_test = false;
    _putenv_s("MELEE_LAG_TEST", "1");
    if (!std::getenv("MELEE_LAG_TEST_KEEP_CARD")) {   // test runs: keep the card given
      std::error_code ec;
      const std::filesystem::path card = std::filesystem::u8path(o.card_dir).parent_path() / "LagTestCard";
      std::filesystem::remove_all(card, ec);
      std::filesystem::create_directories(card, ec);
      o.card_dir = card.u8string();
      explicit_card_dir = true;
    }
    host::log("lag test: Lossless adapter lag test launch; memory card %s", o.card_dir.c_str());
  }
  // Not while watching a replay (a short session of its own) or on a run that records nothing.
  if (std::none_of(argv + 1, argv + argc, [](const char* v) { return std::strcmp(v, "--replay") == 0 || std::strcmp(v, "-i") == 0; }))
    compress_earlier_replays(o.replay_dir);
#ifdef _WIN32
  crash_set_settings_path(gfx.settings_path);
  crash_set_test_streamline_throw(automated && host::test_env::getenv("MELEE_TEST_STREAMLINE_THROW") != nullptr);
#endif
  // The game (melee_game.dll) runs from here until the process ends.
  static std::unique_ptr<gx::Backend>* backend_at_exit = &backend;
  auto shutdown = [](int) {
    host::set_process_ending();
    // The shader and pipeline caches are written while the renderer still runs (the app ends a game
    // that has not exited 5 s after it asked, and a Friends join relaunches the game), but asked for
    // first and waited for last: a slow first save never holds back the Friends Leave below.
    const auto caches_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    const auto caches = ::gx::cache_store::request_flush_all("exit");
    // Join the console link service before tearing down the host/bridge.

       // a Leave just before quitting still leaves the room
    ::gx::cache_store::wait_flush_all(caches, caches_due);   // what is left of the 3 s
    host::report_sim_work();
    sim_profiler::report();   // no-op without --profile
    backend_at_exit->reset();
    { uint64_t silent_ms = 0, underruns = host::audio_underruns(&silent_ms);
      double rate_low = 1.0, rate_high = 1.0; host::audio_rate_range(&rate_low, &rate_high);
      host::log("audio: %llu frames played, %llu blocks dropped, %llu gaps (%llu ms held), clock tracking %+.3f%% to %+.3f%%",
                (unsigned long long)host::audio_pushed_frames(), (unsigned long long)host::audio_dropped_blocks(),
                (unsigned long long)underruns, (unsigned long long)silent_ms, (rate_low - 1.0) * 100.0, (rate_high - 1.0) * 100.0); }
    host::audio_close();
    host::downloads::shutdown();
    host::discord::shutdown();
    host::gcadapter_shutdown();
    host::switchpro_shutdown();
    // Both Source Port builds; nothing happens when no --p2p-* session was started.
    source_p2p::shutdown();   // the result file once more, then the session's thread and socket
    slippi::shutdown();
    host::log_flush();   // the process ends with ExitProcess right after this (guarded, source_host_internal.h)
  };
  // With --p2p-port and --p2p-peer: the session starts connecting now, and the game boots straight
  // into the match scene and waits there for it. Both Source Port builds; without those options
  // this returns at once and the run is what it was.
  source_p2p::start(hidden || headless || scripted);
  sim_profiler::start();   // with --profile: the native game runs on this thread; shutdown() reports
  if (host::exit_requested()) { shutdown(0); host::end_process(0); }
  host::log("startup: starting the game%s", host::startup_mark().c_str());
  const int source_code = source_port::run(shutdown);
  shutdown(source_code);
  host::end_process(source_code);   // everything is saved: do not run the libraries' unload code
}
