# Experimental macOS build

The full host targets x86-64 on macOS 15 or later. Apple Silicon uses Rosetta. Supply a Melee NTSC 1.02 ISO, an x86-64 SDL2 development distribution under `../deps/sdl2`, an x86-64 libusb 1.0 distribution under `../deps/libusb` (`include/libusb-1.0/libusb.h` and `lib/libusb-1.0.0.dylib`; the vendored copy in `port/third_party/libusb` is Windows only), and a matching x86-64 Linux game library built from the fork. Set `MELEE_GAME_LIBRARY` to the path of `melee_game.so`. Its `melee_game.snapexcl` and `Sys/GC/dsp_coef.bin` must be beside the library in the layout produced by the guest build.

Build the native game library on x86-64 Linux with GCC and Ninja. The host checks the library's Squad Strike flag at startup. The host and guest both enable offline Squad Strike by default. To build a smaller variant, configure the guest with `-DMELEE_SQUAD_STRIKE=OFF` and set `MELEE_SQUAD_STRIKE=OFF` for the Mac build and packaging steps. Its ELF loader requires the Linux stack canary calls to be disabled.

```sh
git submodule update --init sourceport/extern/melee
cmake -S sourceport/game -B build-mac-guest -G Ninja -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_BUILD_TYPE=Release -DMELEE_SQUAD_STRIKE=ON \
  -DCMAKE_C_FLAGS=-fno-stack-protector -DCMAKE_CXX_FLAGS=-fno-stack-protector
cmake --build build-mac-guest --target melee_game
```

(Ubuntu under WSL works. The fork needs no `prepare_native_sources.py`; it refuses a `git archive` copy.)
Copy `melee_game.so`, `melee_game.snapexcl` and the `TM` folder (Training Mode CE's data) from that build to the Mac, with `Sys/GC/dsp_coef.bin` (from `port/slippi_sys`) beside them, and set `MELEE_GAME_LIBRARY` to the copied ELF file. The library links all m-ex fighters in: fighter plugins load on Windows only (`mexft_plugins.c` `scan()`).

Dependencies on Apple silicon (2026-11-25, Homebrew 7 has no x86-64 bottles): build them from the sources `brew fetch --build-from-source` downloads. SDL2 is `sdl2-compat` over SDL3, both `-DCMAKE_OSX_ARCHITECTURES=x86_64`; put `libSDL3.0.dylib` beside `libSDL2-2.0.0.dylib` as `libSDL3.dylib` (install name `@rpath/libSDL3.dylib`), where sdl2-compat looks for it and packaging bundles it. libusb: `configure --host=x86_64-apple-darwin CC="clang -arch x86_64"`. Build glslang (`-DENABLE_OPT=OFF -DENABLE_HLSL=ON -DBUILD_SHARED_LIBS=OFF`) and SPIRV-Cross universal (`arm64;x86_64`) so packaging can run them on any Mac.

Running anything x86-64 on Apple silicon needs Rosetta 2 (`softwareupdate --install-rosetta`, an administrator's choice). Without it the host still compiles, links and packages (the seed tool is then built for the Mac itself), but the app and every `check_full.py` check stop with "Bad CPU type in executable". The host's unit tests also build natively: configure the root `CMakeLists.txt` with `-DCMAKE_OSX_ARCHITECTURES=arm64` and run `python3 tools/run_tests.py quick --host <that folder>` (`port_keyboard_navigation` is built x86-64 only on macOS).

Then compile the host.

```sh
python3 port/mac/build_full.py --host-only
```

To package the full app, install `glslangValidator` and `spirv-cross` under `../deps/tools/bin` and run `python3 port/mac/package_full.py`. The bundle includes the host, guest library and runtime assets. The player supplies the ISO separately.

When you open the app in Finder, press **F1** (or **Fn + F1** on a Mac keyboard) to open the in-game settings. In a Training match, press **Tab** to open the Practice menu.

The offline graphics diagnostic has a separate build script, `port/mac/build-app.sh`. It exercises the renderer without loading the full online host.
