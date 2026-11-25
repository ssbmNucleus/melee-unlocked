# Nucleus Unlocked

Nucleus Unlocked is a fork of [Melee Unlocked](https://github.com/Hero88go/melee-unlocked), a native PC port of Super Smash Bros. Melee (NTSC 1.02), built on [Melee Unlocked: Slippi Ranked](https://github.com/0xburn/melee-unlocked-slippi-ranked).

It has one engine, the Source Port: `melee_source.exe` runs `melee_game.dll`, the game compiled from the Melee decompilation. You need your own Melee NTSC 1.02 ISO.

To build it on Windows, run `build.bat`, or follow [docs/guide/build-and-run.md](docs/guide/build-and-run.md) step by step.

## Play

[Download the Windows or Mac zip.](https://github.com/0xburn/melee-unlocked-slippi-ranked/releases/latest) You need your own Melee NTSC 1.02 ISO.

- **Windows x64:** Extract the zip and double-click `melee_source.exe`; it asks for your ISO once and remembers it (or takes `melee.iso` from its folder). You can also start the game from a launcher such as SSBM Nucleus (`melee_source.exe --help` lists the command line). Escape closes the game; the settings are in the Nucleus app and in the separate settings window. Nucleus Client runs directly or through an external launcher.
- **Mac:** Extract the zip, open Nucleus, and select your ISO. Requires macOS 15 or later and Rosetta on Apple Silicon.

For Ranked, sign in to Slippi Launcher with an account that has Ranked access. Choose **Online > Ranked**.

## Build

- **Windows:** Everything builds locally. Build `melee_source.exe` (MSVC 2022) and `melee_game.dll` (MinGW-w64 GCC 14+) as [docs/build-source-port.md](docs/build-source-port.md) describes, then `python windows-build/client_release.py --host-build build-host --game-build build-gcc` makes the release folder and zip ([docs/client-release.md](docs/client-release.md)).
- **Mac:** Follow the [Mac build instructions](port/mac/README.md) to build the game library and package the app.

For controls, features, and other documentation, see the [original Melee Unlocked repository](https://github.com/Hero88go/melee-unlocked).
