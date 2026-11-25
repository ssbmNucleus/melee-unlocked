# The Source Port client release

`windows-build/client_release.py` makes the release of the Source Port client:
`melee_source.exe` and `melee_game.dll` with what they need at run time, without the
launcher and without the Static Recomp (`melee_port.exe`). It writes a manifest of
every file and can check a folder against one, which is what SSBM Nucleus runs after
it downloads a client. It never signs, uploads or publishes anything.

## Toolchains

The client is built by two toolchains:

| Part | Toolchain | CMake source | Outputs |
| --- | --- | --- | --- |
| `melee_source.exe` (host) | MSVC 2022, generator `Visual Studio 17 2022`, `-A x64` | the repository root | `<host build>/port/Release` |
| `melee_game.dll` (game) | MinGW-w64 GCC 14+ with Ninja | `sourceport/game` | `<game build>`: `melee_game.dll`, `.snapexcl`, `.syms`, `.dbg` |

Configure each once as in [Building the Source Port](build-source-port.md) (same
`-DMELEE_CPU_BASELINE` for both), and add `-DNUCLEUS_PUBLIC_BUILD=ON` to both configures:
the script refuses anything else. Never configure a release host with
`-DMELEE_PLUGIN_TEST_KEY=ON`: the script refuses such a build too.

## The public build (`NUCLEUS_PUBLIC_BUILD`)

One CMake option, off by default, in the root `CMakeLists.txt` (the host) and in
`sourceport/game/CMakeLists.txt` (the game library). Off, development builds and every
test behave as they always have. On, it makes the build that goes to players
(`port/runtime/host/public_build.h`):

- **Test-only launch options** (`CliOption::test_only` in `port/app/main.cpp`:
  `--online-test`, `--local-peer`, `--allow-matchmaking`, `--test-stage`,
  `--squad-test-invalid-lineup`) are left out of `--help` and refused with exit 2
  (`--online-test is a test option; this build of Nucleus does not take it`).
- **Test hooks read nothing.** `host::test_env::getenv` and `automated_getenv`
  (`port/runtime/host/test_env.h`) answer nothing, so every `MELEE_TEST_*` diagnostic,
  the setup-hash skew, the forced overlays, the network stand-ins
  (`MELEE_TEST_MM_HOST`, `MELEE_TEST_LOBBY_API`, `MELEE_TEST_MOD_BROWSER_API`) and the
  `MELEE_GAME_DLL` override are ignored; the game's `mu_test_getenv` (`shim/mu_entry.c`)
  answers nothing whatever the host. Every other test switch, trace and dump reads through
  these two as well: early rollback and early inputs (`MELEE_EARLY_*`), early checksums, the
  sync and replay switches, the lab fakes, `MELEE_TRACE_*`, `MELEE_DUMP_*`, the profiler, the
  settings window's screenshot hooks. The raw reads left are this computer's own picture and
  sound tuning, the launch options' own channels (`--lag-test`, `--record-native`), the fighter
  plugin folder, the player's opt-ins the mod browser names (`MELEE_ALLOW_PLUGIN_DOWNLOADS`,
  `MELEE_FIGHTER_TOOLS`) and `MELEE_ASSUME_VRR`; the list with its reasons is
  `port/tests/raw_env_reads_test.py` (quick), which fails on a new raw read.
  `MELEE_AGENT_COPY` still locks an automated copy out of the account and online actions in both
  builds: it only ever takes things away.
- **Compiled out of the game:** the Online menu's Direct row for a signed-out agent copy with
  local peering (`mu_slippi_option_unlocked`) and the rollback self-test
  (`MELEE_ONLINE_SELFTEST` in `mu_online_test_wait`).
- **The log names other players by a short hash** (`id-` and six hex digits,
  `public_build::log_id`, and `mu_log_id` in the game with the same hash): the Direct opponent's
  code when matchmaking starts, a Discord invite's code and a Teams lobby's code; an unexpected
  matchmaking reply is logged by its type alone. The player's own name and code are logged as
  before.
- **`--print-info`** says `"public_build": true`, and `"game_public_build": true` when the
  `melee_game.dll` beside it exports `mu_public_build` (null when no library could be read).

```powershell
& $CMAKE -S sourceport/game -B build-game-public -G Ninja <toolchain flags as usual> -DNUCLEUS_PUBLIC_BUILD=ON
& $CMAKE -S . -B build-host-public -G "Visual Studio 17 2022" -A x64 -DNUCLEUS_PUBLIC_BUILD=ON
```

A public build's tests: `port_skin_fetch` and `port_skin_fetch_view` skip (77) because their
stand-in would not be read and the real site would be asked; `print_info_test.py`,
`production_integrity_test.cpp` and `online_menu_test.py` check the public behavior instead, and
`release_scope_test.cpp` checks that an automated run there gets the release table plus the
settings file (no `MELEE_TEST_MODS_SCOPE`, no "every category").

## Making a release

From a clean, committed tree:

```powershell
python windows-build/client_release.py --host-build build-review --game-build build-sourceport-gcc [--out release/client] [--jobs 3] [--require-dlss]
```

builds `melee_source` and `melee_game` (`cmake --build`, below-normal priority,
`--jobs` at a time; `--no-build` takes the outputs as they are), or, with outputs
built elsewhere:

```powershell
python windows-build/client_release.py --host-dir <folder with melee_source.exe> --game-dir <folder with melee_game.dll>
```

It then

1. runs `tools/fetch_graphics_dlls.py --dxc --librashader` (which checks the pinned
   SHA-256 and downloads only what is not already in place) and ships those libraries;
2. assembles the folder: the host, the game library with
   `melee_game.snapexcl` and `melee_game.syms` (fighter plugins link against it), the
   DLSS runtime when the host was built with Streamline (all of it or none; `--require-dlss`
   makes none an error), the Visual C++ runtime, `Sys`, `SysPlayback`, `deltas`, the CRT shader pack,
   the settings panel's pictures, the license texts (the same list as the full release),
   `tools/xdelta3.exe` and `tools/7zr.exe` when present, and a README (no batch file: a
   double-click on `melee_source.exe` finds the disc itself, asking once);
3. runs the packaged `melee_source.exe --version` (must equal `VERSION`) and
   `--print-info` in the folder (no window, no files written) and refuses a build that
   trusts the test key, lacks plugin approval, names no plugin ABI, or is not a public
   build (`public_build` and `game_public_build` must both be true);
4. writes the manifest into the folder (`release-manifest.json`), zips the folder, and
   writes the manifest again beside the zip with the zip's size and SHA-256.

Output in `--out`:

```
NucleusClient-<version>-win64/                 the release folder
NucleusClient-<version>-win64.zip              the folder zipped (entries under that folder name)
NucleusClient-<version>-win64.manifest.json    the manifest plus "zip": {name, size, sha256}
NucleusClient-<version>-win64-symbols/         melee_game.dbg, for crash reports (not shipped)
```

A tree with uncommitted changes to tracked files is refused unless `--allow-dirty`
(then `git_dirty` is true). Signing, if it is ever wanted, has to happen before the
manifest is written, because a signature changes the files; this script has no
signing step.

## The manifest

```json
{
  "format": 1,
  "kind": "nucleus-client",
  "name": "NucleusClient-1.0.6-win64",
  "version": "1.0.6",
  "git_commit": "<40 hex>",
  "git_dirty": false,
  "created": "2026-11-23T10:00:00Z",
  "toolchains": {"host": "MSVC 19.42.34435.0", "game": "GNU 14.2.0"},
  "plugin_format": 2,
  "plugin_abi": "<32 hex>",
  "notes": ["what optional parts are not in this release, and why"],
  "print_info": {"...": "the packaged melee_source.exe --print-info answer"},
  "files": [{"path": "melee_source.exe", "size": 10167296, "sha256": "<64 hex>"}],
  "zip": {"name": "NucleusClient-1.0.6-win64.zip", "size": 81910151, "sha256": "<64 hex>"}
}
```

`files` lists every file in the folder except `release-manifest.json` itself, sorted,
with forward slashes. `zip` is only in the copy beside the zip. Paths of the build
machine are replaced by `<release>` in `print_info`. `toolchains` says
`prebuilt (--host-dir)` / `prebuilt (--game-dir)` when the outputs came from elsewhere.
`plugin_format` and `plugin_abi` are what a `.mufighter` plugin must be built for.

## Checking a folder

```powershell
python windows-build/client_release.py --verify <folder> [--manifest <file>] [--strict]
```

prints one JSON object:

```json
{"action": "verify", "ok": false, "folder": "...", "manifest": "...", "version": "1.0.6", "git_commit": "...",
 "checked": 192, "strict": false,
 "changed": [{"path": "melee_game.dll", "expected_size": 1, "size": 1, "expected_sha256": "...", "sha256": "..."}],
 "missing": ["licenses/monocypher.md"],
 "extra": ["port-settings.ini"]}
```

Exit code 0: every listed file is there with its size and SHA-256 (extra files are
reported but allowed, since a game folder gains settings, logs and caches once it has
run; `--strict` fails on them too). 1: something changed or is missing. 2: the manifest
cannot be used (unreadable, another kind, or an entry that is not a plain relative path
inside the folder). Without `--manifest` the check uses `release-manifest.json` in the
folder; whoever can change the files can change that too, so a downloader checks
against the manifest it got from its own trusted source.

`windows-build/client_release_test.py` (ctest `client_release`) covers the manifest,
the zip, the refusals and `--verify` (a changed byte, a changed size, a missing file,
an extra file, a rewritten manifest beside the files, unusable manifests).
