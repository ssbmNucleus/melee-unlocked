# Building the Source Port

The Source Port has two components: `melee_game.dll`, compiled from the Melee decomp
and native shims with MinGW-w64 GCC 14 or newer, and `melee_source.exe`, compiled
with MSVC.
Game data comes from your own NTSC 1.02 ISO.

Initialize the pinned public decomp and apply the released native adaptations:

```powershell
git submodule update --init sourceport/extern/melee
python tools/prepare_native_sources.py
```

The adaptations are tracked in `sourceport/patches/melee-native.patch`, including
the native changes that previously existed only in the local submodule. The script
checks the pinned upstream commit, refuses unrelated local edits, and accepts an
already applied patch. Run this preparation command before configuring the native library.
To inspect the adaptations, run `git -C sourceport/extern/melee diff`. Configuring
`sourceport/game` runs the same script with `--check` (it changes nothing) and stops when the
checkout is the public decompilation without the patch.


Install a MinGW-w64 GCC 14+ distribution with Ninja and configure the DLL (replace
the toolchain path with your installation):

```powershell
cmake -S sourceport/game -B build-sourceport-gcc -G Ninja "-DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake" "-DMELEE_MINGW_ROOT=C:/toolchains/mingw64"
cmake --build build-sourceport-gcc --target melee_game --parallel
```

Then configure and build the host:

```powershell
cmake -S . -B build-review -G "Visual Studio 17 2022" -A x64
cmake --build build-review --config Release --target melee_source --parallel
Copy-Item build-sourceport-gcc/melee_game.dll build-review/port/Release/
Copy-Item build-sourceport-gcc/melee_game.snapexcl build-review/port/Release/
build-review/port/Release/melee_source.exe --iso "C:/path/to/melee.iso" --threaded-renderer
```

Use matching CPU baselines for the DLL and host. `-DMELEE_CPU_BASELINE=SSE2` builds
the compatibility variant.

The game DLL is built from source (the Static Recomp, which ran a translation of the retail
code, was removed on 2026-11-24). v0.8.5 exposes TM-CE and 20XX TE under Mods; see
[training-mod controls and limits](training-mods.md).

## Probing a build and its exit codes

`melee_source.exe --print-info` prints one JSON
object on stdout and exits 0 without a disc, a window or a log file: `version`,
`engine` (always `source`), `cpu_baseline` and `cpu_supported` (it answers on a
processor that cannot run the build, too), `game_library`, `plugin_format` and
`plugin_abi` (what a `.mufighter` plugin must be built for, read from the game
library beside the program without running it; `null` when it is not there),
`linked_fighters` (m-ex fighters linked into the library instead of shipped as
plugins), `fighters_folder` (the default installed-fighters folder), and whether
the build takes `--fighters-dir`, `--no-crash-dialog` and `--no-dialogs`
(`fighters_dir`, `no_crash_dialog`, `no_dialogs`). On Windows it also has the
fighter plugin approval fields (`plugin_signing_keys`, `plugin_test_key`,
`plugin_approvals`, `plugins_folder`, `plugins_blocked`; docs/fighter-plugins.md).
It runs before anything else in the process (on Windows from a C runtime
initialiser slot, ahead of every static constructor) and never opens a window: if
reading any of it fails, it prints `{"error": "..."}` and exits 1.

`--no-dialogs` (`--no-crash-dialog` is the same option) means no message
box at all: a refused processor, a disc the start refuses, memory that cannot be
reserved, a missing graphics card, a fatal error or a crash is logged, writes its
report (`melee_port_crash.txt`) and exits with its code below. It is always on for
`--hidden` and `--headless` runs and for the one-shot modes a launcher or a script
runs unattended: `--print-info`, `--version`, `--help`, `--prewarm-only`,
`--card-self-test` and the skin catalog commands (`--import-cosmetic`,
`--scan-disc-skins`, `--select-cosmetic`, `--deselect-cosmetic`,
`--enable-project-effects`, `--restore-vanilla-cosmetics`, `--cosmetic-status`) and
the plugin approval commands (`--approve-plugin`, `--revoke-plugin`, `--list-plugins`;
`port/tests/plugin_approval_test.py` runs those the same way).
`port/tests/print_info_test.py` (ctest `source_print_info`) runs each of them with
stdin closed and fails on one that is still running after 20 seconds or shows a
window. Exit codes, as `--help` lists them:

| Code | Meaning |
| --- | --- |
| 0 | ended normally (Quit, `--frames` reached, `--help`, `--version`, `--print-info`) |
| 1 | could not start: the disc, a replay or an input script could not be opened, or memory could not be reserved |
| 2 | bad command line; a disc this launch cannot use (a modded disc where the online menus need the original, a non-retail disc with `--vanilla-only`); a `--lobby-direct` or online test match that never started |
| 3 | the processor lacks AVX2, which this build needs |
| 4 | a replay was closed before its end (a launcher's replay queue can stop there) |
| 5 | a replay this game cannot play was refused before its match: a fighter, stage or costume it names is not in this game, or it was recorded with installed fighters this game does not have (a different fighter would play its inputs). The log has one line, `replay: ...; the match is not started`, and no recording is written |
| 70 | the game stopped on an error it reported; `melee_port_crash.txt` and the log say which (3 before) |
| other | an exception code such as `0xC0000005`, after a crash the crash handler wrote up |

`--prewarm-only` has its own codes (see `--help`).

`port/tests/replay_refusal_test.py` (ctest `source_replay_refusal`) writes small replays
that name a character, a stage or a costume the game does not have, and one recorded
with two installed fighters, and checks that each exits 5 with its log line, no window,
no crash report and no recording; a retail replay made the same way must start its
match. It needs `MELEE_TEST_ISO` and the game library (beside the program, or named by
`MELEE_GAME_DLL`), and is skipped without them.

## Running the tests

```powershell
python tools/run_tests.py quick     # under a minute, no game run, no disc: host and game build folders
python tools/run_tests.py game      # start melee_source.exe or read the disc (a few minutes)
python tools/run_tests.py slow      # long programs; port_slippi_* (built only on request)
python tools/run_tests.py quick --build   # a fresh worktree: build the label's test programs first
```

`tools/run_tests.py` runs `ctest -L <label>` in `build-host` and, for `quick`, `build-gcc` (the folders
`tools/new_worktree.py` makes; `--host` and `--game` name others) and prints one line per folder:
passed, skipped, failed and the wall time, then each skipped test with its reason and each failed one.
The same in plain CTest: `ctest --test-dir build-host -C Release -L quick -j 8` and
`ctest --test-dir build-gcc -L quick -j 8`. The labels are set in `port/tests/test_labels.cmake`; every
test in the game folder is quick (its programs: `ninja -C build-gcc mu_game_tests`).

`game` finds the disc itself (`MELEE_TEST_ISO`, else `tools/melee_iso.py`) and gives the tests that
open UDP ports this worktree's range (`MU_TEST_BASE_PORT`). Its two match tests:

- `source_match_digest` (`port/tests/match_digest_test.py`): one scripted offline VS match
  (`port/tests/fixtures/digest_vs.txt`, `--match 31:11:13`) played twice side by side with
  `--fast --time-base 0 --rng-seed 0x12345678 --state-digest`. The two digests must agree for 900 match
  frames, and their SHA-256 must be the one recorded in the test. A change meant to alter gameplay
  changes it: `python port/tests/match_digest_test.py <melee_source.exe> --record` prints the new value.
- `source_p2p_pair` (`port/tests/p2p_pair_test.py`): `tools/p2p_pair.py` (Fox against Marth, two
  hidden instances over loopback) must PASS with 0 checksums mismatched.

A test that cannot run on this PC is skipped with its reason, never counted as failed: the source
fixtures that compile game code with GCC or Clang (CMake hands them the MinGW `g++`/`gcc` it finds;
without one they skip), the `port_slippi_*` programs when they have not been built (`slow` only; they
link the whole static runtime; `port/tests/run_if_built.py` names the target to build), and the tests
that need the disc.

What a test here is for: it runs real code (a parser, a validator, a codec, a model, the built program).
A test that only checks that a source file contains some text breaks on a rename that changes nothing
and teaches everyone to ignore red; do not add one.
