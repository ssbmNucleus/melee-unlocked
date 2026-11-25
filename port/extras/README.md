# port/extras: launcher code kept for reuse

Nucleus Client runs without its old launcher (MeleeUnlockedLauncher.exe on Windows,
MeleeUnlocked on Linux). The game is started directly: by double-clicking melee_source.exe, by
MeleeUnlocked.bat, or by an external launcher that runs melee_source.exe with --iso and the other
options listed by `melee_source.exe --help`.

The code in this folder did work for the old launcher that an external launcher may want to do
too. It is kept as it was, with its tests, but **nothing here is built** by this repository's
CMake files and no file here is compiled into the game. Each folder says what it needs.

## What a launcher does now

What the old launcher did for the game, and what an external launcher takes over:

- **The disc.** Pass `--iso <Melee NTSC 1.02 ISO>` every time; the game no longer writes
  `%LOCALAPPDATA%\MeleeUnlocked\launcher.ini`.
  Only a start with no arguments (a double-click) asks for the disc and remembers it in
  `standalone.ini`, in the game's own data folder `%LOCALAPPDATA%\Nucleus Client` (its first start
  copies a profile forward once from `%LOCALAPPDATA%\Netlee`, else from Melee Unlocked's folder, and
  never changes those). `disc/` checks a disc before a start.
- **Replays.** Pass `--replay-dir <folder>` and, for "Save replays compressed", `--compress-replays`.
  A replay closed before its end exits with code 4.
- **Unattended starts.** `--no-dialogs` (no message box at all), `--hidden`, `--print-info`
  (one JSON object: version, engine, CPU support, plugin ABI, fighters folder, accepted options).
- **Fighters.** `--fighters-dir <folder>` for one set of installed fighters per listing.
- **Mod discs.** `melee_source --iso <game> --mod-iso <disc>`, or `melee_port --iso <disc>
  --mod-base-iso <game>`. `Mods/.cache/detected.json` lists what the game found in `Mods`.
- **Updates.** The game never checks for or installs a new version. Each release still carries
  `compat.sha256` (the build for processors without AVX2, see `downloads/`).
- **Crashes.** The game writes `melee_port_crash.txt` and `melee_port_crash.dmp` beside
  `melee_port.log` and exits with the exception code (70 after an error it reported). Collecting or
  sending them is up to the launcher (`crash_report/`).
- **Settings without a game.** `--settings-window`, `--settings-standby <launcher process id>`,
  `--settings-tab <name>`.
- **Online.** `--lobby-direct NAME#123 --lobby-character N` (one Slippi Direct match, then exit)
  and the `--p2p-*` options; routers: `nat/`.
- **Discord "Ask to Join".** The game registers itself as the `discord-<application id>://`
  handler but does not take such a link on its command line (an unknown option exits with code 2);
  a launcher that wants joins to work registers its own handler and starts the game for them.

## disc/ - checking the disc a player picks

`disc_checks.h`, `disc_checks.cpp` (taken out of the old `port/app/launcher.cpp`):

- `verify_iso`: the header check (game id GALE01, revision 2 = NTSC 1.02, full size).
- `base_disc_problem`: why a file cannot be the base disc, in words a player understands:
  a compressed image (RVZ, GCZ, WIA, CISO), another game, another revision, a modded `main.dol`
  (SHA-1 of the retail `main.dol`, the same check the game makes at startup), or an m-ex mod disc
  such as Akaneia (`mex_disc`: its file table names `MxDt.dat`).
- `nkit_image`: an NKit image (trimmed retail disc), which is neither the ISO nor a mod.

Windows, C++17, link `bcrypt`.

## disc_rvz/ - Dolphin RVZ and WIA disc images

`disc_rvz.cpp`, `disc_rvz.h`: reads a Dolphin `.rvz` or `.wia` image back to the full disc image
(ported from Dolphin, GPL-2.0-or-later). The old launcher used it to turn a compressed disc a player
picked into an `.iso`; the game itself reads plain ISOs only and names RVZ/WIA images as such when it
refuses one (`port/runtime/host/mod_scan.cpp`).

It needs Zstandard (`port/third_party/zstd`, still in the tree for the game's `.slpz` replays) and the
LZMA/LZMA2 and bzip2 decoders, moved here with it: `third_party/lzma` (LZMA SDK 25.01, public domain)
and `third_party/bzip2` (bzip2 1.0.8, BSD-style). Include directories: `third_party` here and
`port/third_party`; compile definitions `ZSTD_DISABLE_ASM=1 XXH_NAMESPACE=ZSTD_ BZ_EXPORT`.

Test: `disc_rvz_test.cpp` with `-DMU_RVZ_DATA="<this folder>/data"`; `data/` holds the small
fixtures `rvz_fixtures.py` writes from Dolphin's own encoder logic.

## launch/ - choosing the program to start

- `launcher_engine.h` and `launcher_engine_test.cpp`: whether the Source Port can be offered
  (melee_source.exe with melee_game.dll beside it, in the game folder or a source checkout's
  build output) rather than only the Static Recomp (melee_port.exe); how the old launcher.ini
  `engine=` and `vanilla=` lines were read; the arguments a "Vanilla" start adds.
- `cpu_baseline.h`: AVX2 detection. The ordinary build needs AVX2; on a processor without it
  `melee_source.exe` exits with code 3 (and `--print-info` answers `"cpu_supported": false`
  without starting). A launcher starts `melee_source_compat.exe` there when it is present.

## downloads/ - the build for processors without AVX2

`compat_build.h`: how the old launcher fetched `melee_source_compat.exe` and its companions on a
processor without AVX2. A release package still carries `compat.sha256` beside the game (the file
name `MeleeUnlocked-<version>-compat-win64.zip` and its SHA-256), and the release still publishes that
zip; the game itself never downloads it. The header reads and checks the record, says whether the
build beside the game is missing or out of date (`compat.installed`), and builds the address the old
launcher used; the steps after that (verify, unpack with `port/runtime/host/archive_unpack.h`, place,
write `compat.installed`) are listed at its top.

The game's own optional download (AMD FSR, from AMD's repository) stays in the game:
`port/runtime/host/optional_downloads.cpp`, behind its button in the game's own menus.

## mod_catalog/ - installing catalog mods

`launcher_mod_catalog.cpp`, `.h` and `mod_catalog_test.cpp`: the old launcher's Mods page engine,
without its window and without its catalog. It parses a catalog of mods (id, name, credits, licence,
release page, pinned SHA-256 of the download, the patch and the patching tool, the finished disc's
SHA-256), refuses a download whose GitHub digest disagrees with a pin, applies an xdelta patch only
with a tool and patch whose hashes match (output staged, cancel leaves nothing behind), copies and
removes managed packs in `Mods/Discs` and `Mods/Saves`, and switches a pack on or off in the game's
settings file without touching other lines. Windows, C++17, nlohmann/json, link `bcrypt`.

The catalog itself (`mods-catalog.json`, `mods-catalog-2.json`, fetched from raw.githubusercontent.com,
with an offline copy inside the old launcher) is removed: a launcher brings its own. The game still
scans `Mods` itself (`port/runtime/host/mod_scan.cpp`), lists what it found in its own MODS tab and
writes `Mods/.cache/detected.json` for a launcher to read. A mod disc is played by starting the
game with it: `melee_source --iso <game> --mod-iso <mod disc>` on the Source Port, or
`melee_port --iso <mod disc> --mod-base-iso <game>` on the Static Recomp.

## The friends lobby (not kept here)

The old launcher's Multiplayer Lobby (friends, presence, chat and match requests over the public
BitTorrent DHT, `port/app/launcher_lobby*.cpp`, `lobby/`, `port/third_party/dht`) is removed, not
kept: it was tied to the launcher's window and identity store. It is in this repository's history
before its removal (for example `git show 1190f980:lobby/README.md`).

What the game still offers a launcher for online play: `--lobby-direct NAME#123 --lobby-character N`
starts one Slippi Direct match at boot and exits when it ends (0) or never starts (2); the `--p2p-*`
options start a peer-to-peer match (`port/runtime/mu_net`, see `melee_source --help`). Online rooms
(arenas) are in the game's own menus. The game no longer writes a lobby status file.

## nat/ - getting through home routers

`nat_discovery.cpp`, `.h` and `nat_discovery_test.cpp` (moved out of `port/runtime/mu_net` and out of
the game's `mu_net_test.cpp`): what the old launcher's lobby did for its UDP socket before a
peer-to-peer match. STUN (RFC 5389 Binding Request to two public servers, by default
`stun.l.google.com:19302`, `stun.cloudflare.com:3478` and `stun1.l.google.com:19302`, to learn the
outside address and whether the router gives every destination its own port), a port mapping asked
of the home router (NAT-PMP, then UPnP IGD, one-hour lease renewed at half time), and the order of
the candidate addresses handed to the game. `docs/mu-net-protocol.md` section 15 describes it.

The game itself only dials the candidates it is given (`--p2p-*`, `port/runtime/mu_net/peer_connector.cpp`);
the inline address checks it shares with this code (`ip_is_public` and the others) are copied into
`port/runtime/mu_net/ip_class.h`. Windows, C++17, link `ws2_32`, `iphlpapi` and `bcrypt`; no other
mu_net header is needed.

## replay_data/ - reading .slp replay metadata

`launcher_replay_data.cpp`, `.h`, `launcher_replay_lag.inl`, `launcher_replay_stats.inl` and
`launcher_replay_data_test.cpp`: what the old launcher's Replays page showed for each replay
(players, characters, stage, duration, result, per-player stats, the landing lag table from
slippi-js, LGPL-3.0-or-later). Reads `.slp` and `.slpz` (`port/app/replay_slpz.cpp`, which the game
still builds as `port_slpz`). Needs nlohmann/json (`port/third_party/nlohmann`).

**Known problem, not fixed:** the metadata reader crashes on crafted input. `from_ubjson` follows
UBJSON container nesting and element counts from the file without any limit, so a replay made to
nest deeply or to claim huge counts exhausts the stack or memory. Bound the nesting depth and the
counts against the bytes left before using this on replays from other people.

## crash_report/ - building a crash report a player can share

`launcher_crash_zip.h`, `launcher_crash_text.h`, `launcher_crash_privacy.h` and
`launcher_crash_zip_test.cpp`, with `privacy_vectors.json`:

- which files go into a report (`melee_port_crash.txt`, `melee_port.log`, `melee_port.prev.log`,
  on Linux `melee_port.stderr.log`, and the old launcher's own logs) and how much of each, as a
  plain stored zip under an 8 MB cap (over it, the largest file goes first; the crash text is kept
  last and then only its start). The minidump is not included: it stays on the player's computer;
- the privacy scrubber: per line, account/lobby/peer lines are dropped, absolute paths reduced to
  their last component, connect codes, IP and email addresses and user or computer names replaced.
  `tools/crash_report_privacy.py` applies the same rules; both are checked against
  `privacy_vectors.json`.

The game itself still writes `melee_port_crash.txt` and `melee_port_crash.dmp` beside
`melee_port.log` in the game folder after a crash; sending them anywhere is up to the launcher or the
player. The old launcher uploaded them to a relay service that is gone; there is no upload address.
