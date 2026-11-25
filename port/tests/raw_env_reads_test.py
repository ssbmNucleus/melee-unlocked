"""No new raw MELEE_* environment read (quick, no build needed).

A test switch, trace or dump reads the environment through the one choke point of its side:
host::test_env::getenv (port/runtime/host/test_env.h) in the host, mu_test_getenv
(sourceport/game/include/mu_test_env.h) in the game library. A public build (NUCLEUS_PUBLIC_BUILD)
answers nothing there, so no such switch can change a player's game. This test fails when a raw read
of a "MELEE_..." name (getenv, std::getenv, _wgetenv, _dupenv_s, GetEnvironmentVariable) appears in
port/ or sourceport/game/ outside ALLOWED below: route the new read through the choke point, or add
it to ALLOWED under the reason that fits (a launch option's channel, a player-facing hint, this
computer's own picture and sound, another platform's host).

Not scanned: port/tests (the tests themselves), port/third_party, and the decompiled game
(sourceport/extern/melee, a submodule with its own history).
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCANNED = ("port", os.path.join("sourceport", "game"))
SKIPPED = {os.path.join("port", "tests"), os.path.join("port", "third_party"), os.path.join("sourceport", "extern")}
EXTENSIONS = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".inl", ".m", ".mm")

# A call with a literal MELEE_ name, not through test_env:: or mu_test_ (the word before it decides).
RAW = re.compile(r'(?<![\w:])(?:std::|::)?(?:getenv|_wgetenv|GetEnvironmentVariable[AW]?|_dupenv_s)\s*\(\s*'
                 r'(?:[\w&]+\s*,\s*[\w&]+\s*,\s*)?(?:L|u8)?"(MELEE_\w*)"')


def files(*paths):
    return set(paths)


# Each name a raw read may take, and the files it may be read in.
ALLOWED = {
    # Launch options and the host's own channels: the host sets these itself (--lag-test, --record-native
    # and the spectator recording, a restart after a lost device) or a player names a folder with them.
    "MELEE_LAG_TEST": files("port/app/main.cpp", "port/app/source_host_card.cpp", "port/runtime/gx/settings_file.cpp",
                            "port/runtime/gx/settings_page_video.cpp", "port/runtime/hle/exi_slippi.cpp",
                            "port/runtime/host/gc_adapter.cpp", "port/runtime/host/host_boot.cpp",
                            "port/runtime/host/window.cpp", "sourceport/game/shim/mu_lagtest.c"),
    "MELEE_LAG_TEST_KEEP_CARD": files("port/app/main.cpp", "port/app/source_host_card.cpp"),
    "MELEE_LAGTEST_FLASH": files("port/runtime/host/host_timing.cpp"),
    "MELEE_SOURCE_RECORD": files("port/app/source_host_boot.cpp", "port/app/source_host_replays.cpp",
                                 "sourceport/game/shim/mu_replay.c"),
    "MELEE_DEVICE_LOST": files("port/runtime/host/host_exit.cpp", "port/runtime/host/host_log.cpp"),
    "MELEE_DEVICE_LOST_WHY": files("port/runtime/host/host_log.cpp"),
    "MELEE_FIGHTER_PLUGINS": files("port/app/plugin_gate.cpp", "sourceport/game/mexft/mexft_plugins.c"),
    # A player's own opt-in that the mod browser names in its messages: fighter code downloads and the
    # tools to build a fighter from source.
    "MELEE_ALLOW_PLUGIN_DOWNLOADS": files("port/runtime/host/mod_browser.cpp"),
    "MELEE_FIGHTER_TOOLS": files("port/runtime/host/mod_browser.cpp"),
    # A player-facing display hint (a VRR display the detection misses).
    "MELEE_ASSUME_VRR": files("port/runtime/gx/settings_file.cpp", "port/runtime/gx/threaded_backend.cpp"),
    # Only ever takes something away (no adapter polling), for automated runs and their visible windows.
    "MELEE_NO_GC_ADAPTER": files("port/app/main.cpp", "port/runtime/gx/gx_d3d12.cpp", "port/runtime/gx/texture_pack.cpp",
                                 "port/runtime/gx/threaded_backend.cpp", "port/runtime/gx/user_shader.cpp",
                                 "port/runtime/hle/jukebox.cpp", "port/runtime/host/cosmetic_serve.cpp",
                                 "port/linux/host_services.cpp"),
    # Read before the C runtime starts (GetEnvironmentVariableW); the code checks public_build::kOn itself.
    "MELEE_TEST_PRINT_INFO_FAULT": files("port/app/early_start.cpp"),
    # This computer's own picture, pacing and sound: renderer, presentation and audio tuning and their
    # diagnostics. None of them reaches the simulation, an online match or another player.
    "MELEE_PLAN_CACHE_CHECK": files("port/runtime/gx/gx_d3d11.cpp"),
    "MELEE_UPLOAD_EVERY_PRESENT": files("port/runtime/gx/gx_d3d11.cpp", "port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_BACKLOG_FULL": files("port/runtime/gx/gx_d3d11.cpp", "port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_UI_DIAG": files("port/runtime/gx/gx_d3d12.cpp", "port/runtime/gx/pc_settings.cpp", "port/runtime/gx/phob_menu.cpp"),
    "MELEE_D3D12_NO_WAITABLE": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_D3D12_VALIDATION": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_DLSS_SUPERSAMPLE": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_DLSS_BIAS_EXTRA": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_DEBUG_MVEC_ALL": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_FSR_JITTER_SIGN": files("port/runtime/gx/gx_d3d12.cpp", "port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_DEBUG_DLSS_INPUT_ODD": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_DEBUG_HUDMASK": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_GPU_STARTUP_DIAG": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_FSR_FG_SHOW_GENERATED": files("port/runtime/gx/gx_d3d12.cpp", "port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_WARMUP_MONOTONIC": files("port/runtime/gx/gx_d3d12.cpp"),
    "MELEE_FSR_MV_SCALE": files("port/runtime/gx/gx_fsr_vk.cpp"),
    "MELEE_DLSS_JITTER_HUD": files("port/runtime/gx/gx_shader.cpp"),
    "MELEE_SL_NO_FG": files("port/runtime/gx/gx_streamline.cpp"),
    "MELEE_SL_FORCE_FG": files("port/runtime/gx/gx_streamline.cpp"),
    "MELEE_DLSS_PRESET": files("port/runtime/gx/gx_streamline.cpp"),
    "MELEE_VULKAN_PRESENT_WAIT": files("port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_VULKAN_VALIDATION": files("port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_VULKAN_PUSH_DESCRIPTORS": files("port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_VULKAN_SYNC_PIPELINES": files("port/runtime/gx/gx_vulkan.cpp"),
    "MELEE_RTX_SEMANTICS": files("port/runtime/gx/rtx_semantics.cpp"),
    "MELEE_SUBFRAME_DIAG": files("port/runtime/gx/subframe.cpp"),
    "MELEE_AUDIT_STAGE_SPLIT": files("port/runtime/gx/subframe.cpp"),
    "MELEE_ROLLBACK_HOLD": files("port/runtime/gx/subframe.cpp"),
    "MELEE_SOLVER_WORKERS": files("port/runtime/gx/subframe.cpp"),
    "MELEE_PAIR_PARALLEL": files("port/runtime/gx/subframe.cpp"),
    "MELEE_UNCAPPED_HOLD": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_PHASE_TIMELINE": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_PRESENT_ON_HANDOVER": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_PRESENT_LOCK": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_SHOWN_LEAD": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_STALE_DISCARD": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_SUBMIT_CANCEL": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_ARRIVAL_EXACT": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_ARRIVAL_GLIDE": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_HANDOVER_SPIN": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_SCANLINE_SYNC": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_KEYFRAME_MODE": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_INTERP_TIMELINE": files("port/runtime/gx/threaded_backend.cpp"),
    "MELEE_PAD_THREAD": files("port/runtime/host/window.cpp"),
    "MELEE_AUDIO_PACING": files("port/app/source_host_console.cpp", "port/runtime/host/host.cpp",
                                "port/runtime/host/host_timing.cpp"),
    "MELEE_AUDIO_PACING_TRACE": files("port/app/source_host_console.cpp", "port/runtime/host/host_timing.cpp"),
    "MELEE_AUDIO_PHASE_US": files("port/app/source_host_console.cpp"),
    "MELEE_AUDIO_EARLY_KEYON": files("port/app/source_host_options.cpp"),
    "MELEE_FIGHTER_SOUNDS": files("port/app/source_host_roster.cpp"),
    "MELEE_AUDIO_DMA_PUSH": files("port/runtime/hle/audio_core.cpp", "port/runtime/hle/hle_stubs.cpp"),
    "MELEE_AUDIO_FRESH_CHECK": files("port/runtime/hle/audio_core.cpp", "port/runtime/hle/hle_stubs.cpp"),
    "MELEE_AUDIO_OLD_ACC": files("port/runtime/hle/ax_ucode.cpp"),
    "MELEE_AUDIO_ITD": files("port/runtime/hle/ax_ucode.cpp"),
    "MELEE_AUDIO_POLYPHASE": files("port/runtime/hle/ax_ucode.cpp"),
    "MELEE_DSP_COEF": files("port/runtime/hle/ax_ucode.cpp", "port/linux/main.cpp", "port/mac/main.cpp"),
    "MELEE_AUDIO_FLOOR_TRIM": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_TRACE": files("port/runtime/host/audio.cpp", "port/linux/full/audio_sdl.cpp"),
    "MELEE_AUDIO_DUMP_VOLUME": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_OLD_CLOCK": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_CUBIC": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_EXCL_PUSH": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_MIN_PERIOD_MS": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_TWO_STAGE": files("port/runtime/host/audio.cpp"),
    "MELEE_AUDIO_DEVICE_DUMP": files("port/runtime/host/audio.cpp"),
    # Other platforms' hosts and their diagnostics (not part of the Windows player build).
    "MELEE_DEBUG_OUTPUT": files("port/linux/compat/compat.cpp"),
    "MELEE_NO_JUKEBOX": files("port/linux/host_services.cpp"),   # sets it for its own game library
    "MELEE_SHADER_SEED": files("port/linux/renderer_gl.cpp", "port/mac/full_launch.h", "port/mac/gx_metal.mm"),
    "MELEE_SYNC_SHADERS": files("port/linux/renderer_gl.cpp"),
    "MELEE_GL_COMPARE_CULLING": files("port/linux/renderer_presenter.cpp"),
    "MELEE_MAC_COMPAT_TEST": files("port/mac/compat_test.cpp"),
    "MELEE_METAL_ARCHIVE": files("port/mac/gx_metal.mm"),
    "MELEE_MAC_DEV_SPIKE": files("port/mac/main.cpp"),
    "MELEE_PAUSE_PROBE": files("port/runtime/host/pause_probe.h"),   # the Mac pause diagnostics only
}


def scan(root):
    """{(path, name): [line numbers]} of the raw reads under root (paths with forward slashes)."""
    found = {}
    for top in SCANNED:
        for folder, dirs, names in os.walk(os.path.join(root, top)):
            rel_folder = os.path.relpath(folder, root)
            dirs[:] = [d for d in dirs if os.path.join(rel_folder, d) not in SKIPPED and not d.startswith(".")]
            for name in names:
                if not name.endswith(EXTENSIONS):
                    continue
                path = os.path.join(folder, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="replace") as source:
                    for number, line in enumerate(source, 1):
                        for match in RAW.finditer(line):
                            found.setdefault((rel, match.group(1)), []).append(number)
    return found


def self_check():
    """The pattern still sees what it is for, and nothing routed through a choke point."""
    seen = ['const char* v = getenv("MELEE_X");', 'std::getenv("MELEE_X")', '::getenv("MELEE_X")',
            'GetEnvironmentVariableW(L"MELEE_X", buf, n)', 'GetEnvironmentVariableA("MELEE_X", buf, n)',
            '_wgetenv(L"MELEE_X")', '_dupenv_s(&value, &size, "MELEE_X")', 'mu_crt_x(getenv( "MELEE_X"))']
    unseen = ['host::test_env::getenv("MELEE_X")', 'test_env::getenv("MELEE_X")', 'mu_test_getenv("MELEE_X")',
              'automated_getenv("MELEE_X")', 'h->test_getenv("MELEE_X")', 'getenv(name)', 'getenv("YOUGAME_X")']
    for text in seen:
        assert RAW.search(text), f"the scan misses {text!r}"
    for text in unseen:
        assert not RAW.search(text), f"the scan takes {text!r} for a raw read"


def main():
    self_check()
    found = scan(ROOT)
    if not found:
        print(f"FAIL no source found under {ROOT}")
        return 1
    new = sorted((path, name, lines) for (path, name), lines in found.items()
                 if path not in ALLOWED.get(name, ()))
    stale = sorted((name, path) for name, paths in ALLOWED.items() for path in paths if (path, name) not in found)
    for name, path in stale:
        print(f"note: {path} no longer reads {name}; its ALLOWED entry can go")
    if new:
        for path, name, lines in new:
            print(f"FAIL {path}:{','.join(map(str, lines))}: raw read of {name}")
        print("Read test switches, traces and dumps through host::test_env::getenv (host) or mu_test_getenv "
              "(game library), which a public build answers with nothing; or add the read to ALLOWED in "
              f"{os.path.relpath(__file__, ROOT)} under the reason that fits.")
        return 1
    print(f"ok   {sum(len(v) for v in found.values())} raw MELEE_ reads, all allowed "
          f"({len(found)} name and file pairs); every other switch goes through test_env or mu_test_getenv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
