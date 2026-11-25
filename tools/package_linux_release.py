#!/usr/bin/env python3
"""Package the Linux release: release/NucleusClient-<version>-linux-x86_64.tar.gz.

The same game files as the Windows client zip (windows-build/client_release.py: Sys, SysPlayback,
deltas, the CRT shader pack, the settings appearance assets, Training Mode CE's TM/ files, the
license texts), with the Linux Source Port, its game library (melee_game.snapexcl beside it; the
crash symbols melee_game.dbg stay out of the archive) and, in lib/, every library they need that
differs between distributions, so the archive runs on any x86-64 desktop without installing
anything. The archive's root folder has the archive's name. Nucleus.sh (port/linux/Nucleus.sh)
starts the game with the folder's own places and asks for the disc the first time.

Run inside Linux (WSL is fine) after tools/build_linux.py:
    python3 tools/package_linux_release.py --skip-compat     # AVX2 only, as the Windows client
    python3 tools/package_linux_release.py                   # also the SSE2 compatibility build
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
EXE = 'melee_' + 'source'

# Libraries the system must provide itself: the C runtime and the loader, the graphics driver stack,
# the display server's client libraries, the sound server's, and udev (from AppImage's exclude list).
SYSTEM_LIBRARIES = (
    'linux-vdso', 'ld-linux', 'libc.so', 'libm.so', 'libdl.so', 'libpthread.so', 'librt.so', 'libresolv.so',
    'libutil.so', 'libanl.so', 'libnsl.so', 'libBrokenLocale', 'libmvec.so',
    'libGL.so', 'libGLX', 'libGLdispatch', 'libEGL', 'libOpenGL', 'libGLESv', 'libdrm', 'libgbm', 'libglapi',
    'libX11', 'libxcb', 'libXau', 'libXdmcp', 'libXext', 'libXrender', 'libXrandr', 'libXcursor', 'libXi',
    'libXinerama', 'libXss', 'libXxf86vm', 'libXfixes', 'libxkbcommon', 'libwayland', 'libdecor',
    'libasound', 'libpulse', 'libpipewire', 'libjack', 'libudev', 'libsystemd', 'libdbus', 'libuuid',
    'libfontconfig', 'libfreetype', 'libexpat', 'libz.so', 'libgcc_s.so',
)

README = """Nucleus Unlocked {version} for Linux
{underline}

Super Smash Bros. Melee NTSC 1.02 as a native Linux program, compiled from the game's C source
(melee_source with melee_game.so): the same game as the Windows client, with Slippi online,
replays, the Mods menu, skins and music packs. You need your own Melee NTSC 1.02 disc image; nothing
from the game is included. Not affiliated with, endorsed by, or supported by Nintendo, HAL
Laboratory or the Slippi team. This is the first Linux release.

What you need

  - A 64-bit (x86-64) Linux desktop (X11, or Wayland with XWayland) with glibc {glibc} or newer:
    Ubuntu {ubuntu} or newer, or another distribution of the same age.{too_old}
{cpu}
  - A graphics driver with OpenGL 4.5: Mesa (Intel, AMD) or NVIDIA's own driver.
  - Your own Melee NTSC 1.02 disc image (an .iso, game ID GALE01, revision 1.02). Other versions,
    PAL discs and compressed images are refused.
  - zenity or kdialog to choose the disc in a window (most desktops have one). Without them, start
    the game with the disc's path.
  Every other library the game needs is in lib/ beside it; nothing has to be installed.

Start

  Unpack this folder anywhere you can write to, then from a terminal in it:

    ./Nucleus.sh                      the first time it asks for your disc (or takes melee.iso from
                                      this folder) and remembers it
    ./Nucleus.sh /path/to/melee.iso   that disc
    ./Nucleus.sh --settings           the settings window (video, audio, controllers ...)

  Escape closes the game. The settings are not in the game window: change them in the settings
  window. ./melee_source --help lists the command line for a launcher or a script.

Controllers

  Xbox-style, PlayStation and Switch Pro pads and other SDL game controllers work at once. For a
  GameCube controller adapter (Nintendo's, or one in Wii U mode), give your desktop user access
  once, then unplug and reconnect it:

    sudo install -m 644 51-melee-unlocked-gc-adapter.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules

Music packs

  Settings window > Audio > turn on Use custom music > Open music packs. Tracks.txt there lists the
  original track names. For /audio/name.hps, put your song at MusicPacks/audio/name.mp3 (or .wav,
  .m4a, .flac, .ogg, or stereo .hps). For random picks, put several songs in
  MusicPacks/audio/name/; one folder named name+othername serves both tracks, such as a stage's main
  and alternate songs. A direct replacement comes before a playlist. Extract ZIP packs first: audio
  goes directly inside MusicPacks. Packs replace music, not voices or sound effects.

Saves, settings and logs

  They stay in this folder: User/ (memory card, Slippi sign-in, the disc you chose),
  port-settings.ini, Replays/, Mods/, melee_port.log. After a crash, melee_port_crash.txt is written
  here as well. Two Slippi menu files the game rebuilds from your disc go to
  ~/.local/share/MeleeUnlocked/.

Credits and licensing

  Nucleus Unlocked is built from Melee Unlocked by Hero88go and the Melee Unlocked contributors
  (https://github.com/Hero88go/melee-unlocked), by way of Melee Unlocked: Slippi Ranked by 0xburn.
  Melee Unlocked is free software under the GNU General Public License, version 3 or (at your
  option) any later version: licenses/COPYING.txt, with the notices in licenses/NOTICE.txt. The
  libraries in lib/ and the programs in bin/ and tools/ keep their own licenses, in licenses/.
"""

CPU_AVX2 = """  - A processor with AVX2: Intel Core 4th generation (Haswell, 2013) or newer, or any AMD Ryzen.
    Nucleus.sh says so plainly on a processor without it."""
CPU_COMPAT = """  - Any x86-64 processor. On one without AVX2 (older than Intel Haswell, 2013, or AMD Ryzen),
    Nucleus.sh starts the compatibility build beside the game, melee_source_compat."""
# The oldest Ubuntu release with each glibc, for the README's plain-words requirement.
UBUNTU_GLIBC = ((2, 39, '24.04'), (2, 35, '22.04'), (2, 31, '20.04'), (2, 27, '18.04'))
LAUNCHER = ROOT / 'port/linux/Nucleus.sh'

UDEV = (ROOT / 'port/linux/51-melee-unlocked-gc-adapter.rules')


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def ldd(path):
    out = subprocess.run(['ldd', str(path)], capture_output=True, text=True).stdout
    return {k: v for k, v in ldd_text(out).items() if v != 'not found'}


def ldd_text(out):
    libs = {}
    for line in out.splitlines():   # paths may hold spaces: "name => /a b/name (0x...)"
        line = line.strip()
        if ' => not found' in line:
            libs[line.split(' => ')[0]] = 'not found'
        elif ' => /' in line:
            name, rest = line.split(' => ', 1)
            libs[name] = rest.rsplit(' (0x', 1)[0]
        elif line.startswith('/'):
            path = line.rsplit(' (0x', 1)[0]
            libs[os.path.basename(path)] = path
    return libs


def bundled(name):
    return not any(name.startswith(prefix) for prefix in SYSTEM_LIBRARIES)


def min_glibc(binaries):
    """The newest GLIBC_x.y symbol version any of the binaries asks for: the oldest glibc they all run on."""
    newest = (2, 0)
    for binary in binaries:
        listing = subprocess.run(['objdump', '-T', str(binary)], capture_output=True, text=True).stdout
        for major, minor in re.findall(r'\bGLIBC_(\d+)\.(\d+)', listing):
            newest = max(newest, (int(major), int(minor)))
    return newest


def shader_pak():
    shaders = ROOT / 'port/third_party/slang-shaders'
    if not (shaders / 'crt').is_dir():
        raise SystemExit(f'missing shader presets: {shaders}')
    entries = sorted((p.relative_to(shaders).as_posix(), p.read_bytes()) for p in shaders.rglob('*') if p.is_file())
    digest = hashlib.sha256()
    for entry, data in entries:
        digest.update(entry.encode('utf-8') + b'\0' + data)
    pak = bytearray(b'MUSHPAK1' + digest.hexdigest()[:16].encode('ascii') + struct.pack('<I', len(entries)))
    for entry, data in entries:
        raw = entry.encode('utf-8')
        pak += struct.pack('<H', len(raw)) + raw + struct.pack('<I', len(data)) + data
    return bytes(pak)


def package_snapexcl(source: Path, library: Path, original: Path):
    """Only non-loaded debug sections change in packaging; offsets stay fixed.

    Bind the build's offsets to the final shipped file size, after objcopy/strip.
    Windows retains its existing build-time size contract.
    """
    lines = source.read_text().splitlines()
    sizes = [line for line in lines if line.startswith('# dll_size ')]
    if len(sizes) != 1 or sizes[0] != f'# dll_size {original.stat().st_size}':
        raise SystemExit(f'{source}: dll_size does not match the build library')
    library.with_suffix('.snapexcl').write_text('\n'.join(
        f'# dll_size {library.stat().st_size}' if line.startswith('# dll_size ') else line
        for line in lines) + '\n')


def ship_vulkan_cache(source, target):
    """The warmed Vulkan SPIR-V and its recipes (as tools/package_release.py ships them for Windows):
    SPIR-V is the same on every GPU and driver, so a first Linux session on Vulkan builds its common
    pipelines before the first frame. The recipes ship as vk_recipes.shipped.bin, which an extracted
    update never writes over the player's own vk_recipes.bin."""
    recipes = source / 'vk_recipes.bin'
    if not recipes.is_file():
        print(f'vulkan cache: none at {source} (first Vulkan sessions compile their shaders)')
        return 0
    target.mkdir(parents=True, exist_ok=True)
    count = 0
    for spv in sorted(source.glob('*.spv')):
        shutil.copy2(spv, target / spv.name)
        count += 1
    shutil.copy2(recipes, target / 'vk_recipes.shipped.bin')
    print(f'vulkan cache: {count} SPIR-V files and {recipes.stat().st_size} bytes of recipes from {source}')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-linux/port/linux/full')
    parser.add_argument('--game-build-dir', type=Path, default=ROOT / 'build-linux-game')
    # The compatibility build for processors without AVX2 (tools/build_linux.py --cpu SSE2): without it
    # those players' game stopped on its first instruction with code 132.
    parser.add_argument('--compat-build-dir', type=Path, default=ROOT / 'build-linux-compat/port/linux/full')
    parser.add_argument('--compat-game-build-dir', type=Path, default=ROOT / 'build-linux-game-compat')
    # The Windows client ships no compatibility build (windows-build/client_release.py); this matches it.
    # Its netplay against AVX2 builds has not been checked.
    parser.add_argument('--skip-compat', action='store_true',
                        help='ship only the AVX2 build (Nucleus.sh refuses a processor without AVX2 with a message)')
    parser.add_argument('--out', type=Path, default=ROOT / 'release')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('run this inside Linux (WSL is fine)')
    version = (ROOT / 'VERSION').read_text().strip()
    compat = not args.skip_compat
    exe = args.build_dir / EXE
    game = args.game_build_dir / 'melee_game.so'
    snapexcl = args.game_build_dir / 'melee_game.snapexcl'
    compat_exe = args.compat_build_dir / (EXE + '_compat')
    compat_game = args.compat_build_dir / 'melee_game_compat.so'
    compat_snapexcl = args.compat_game_build_dir / 'melee_game.snapexcl'
    for need in (exe, game, snapexcl, LAUNCHER) + ((compat_exe, compat_game, compat_snapexcl) if compat else ()):
        if not need.is_file():
            raise SystemExit(f'missing {need} (run tools/build_linux.py first, or pass --skip-compat)')
    reported = subprocess.run([str(exe), '--version'], capture_output=True, text=True, timeout=60).stdout.strip()
    if reported != version:
        raise SystemExit(f'the Source Port reports {reported!r}, not {version!r}')
    if compat:
        reported = subprocess.run([str(compat_exe), '--version'], capture_output=True, text=True, timeout=60).stdout.strip()
        if reported != version:
            raise SystemExit(f'the compatibility build reports {reported!r}, not {version!r}')
        for binary in (compat_exe, compat_game):
            listing = subprocess.run(['objdump', '-d', '--no-show-raw-insn', str(binary)], capture_output=True, text=True).stdout
            if any(' %ymm' in line or ',%ymm' in line or 'vfmadd' in line for line in listing.splitlines()):
                raise SystemExit(f'{binary} uses AVX instructions; the compatibility build must not')
    # The release links no added fighter's code (-DMEXFT_PLUGINS=ALL, tools/build_linux.py): Linux loads
    # no fighter plugins, so nothing an installed fighter brings can run code of a different fighter.
    linked = args.game_build_dir / 'mexft/abi/mexft_linked.h'   # generated by sourceport/game/CMakeLists.txt
    if not linked.is_file() or '#define MEXFT_LINKED_FIGHTERS ""' not in linked.read_text():
        raise SystemExit(f'{game} links added fighters ({linked}): configure the game with -DMEXFT_PLUGINS=ALL')

    name = f'NucleusClient-{version}-linux-x86_64'
    stage = args.out / 'linux-stage'
    folder = stage / name
    if stage.exists():
        shutil.rmtree(stage)
    folder.mkdir(parents=True)

    # The programs, without their debug information; the game library's goes to melee_game.dbg, linked
    # back to it, as the Windows build's does (addr2line -e melee_game.dbg reads crash offsets).
    shutil.copy2(exe, folder / EXE)
    run('strip', '--strip-unneeded', folder / EXE)
    # The game build already split them (sourceport/game/CMakeLists.txt). The debug file's sections are
    # compressed (a third of the size; gdb and addr2line read them as is), then the library's debug link
    # is written again so it carries the compressed file's CRC. Only that non-loaded section changes.
    dbg = args.game_build_dir / 'melee_game.dbg'
    if not dbg.is_file():
        raise SystemExit(f'missing {dbg} (built with melee_game)')
    shutil.copy2(game, folder / 'melee_game.so')
    shutil.copy2(dbg, folder / 'melee_game.dbg')
    run('objcopy', '--compress-debug-sections=zlib', folder / 'melee_game.dbg')
    subprocess.run(['objcopy', '--remove-section=.gnu_debuglink', '--add-gnu-debuglink=melee_game.dbg',
                    'melee_game.so'], cwd=folder, check=True)
    # The compatibility pair, for processors without AVX2. Its debug file stays
    # out of the archive (the AVX2 one is most of its size); the link to the other one is removed.
    if compat:
        shutil.copy2(compat_exe, folder / (EXE + '_compat'))
        shutil.copy2(compat_game, folder / 'melee_game_compat.so')
        run('strip', '--strip-unneeded', folder / (EXE + '_compat'))
        run('objcopy', '--remove-section=.gnu_debuglink', folder / 'melee_game_compat.so')
    # Training Mode CE's data files, which the game build writes into TM/ beside the library (as the
    # Windows client ships them): Event Match opens TM-CE with them.
    training = sorted((args.game_build_dir / 'TM').glob('*.dat'))
    if not training:
        raise SystemExit(f'no TM/*.dat in {args.game_build_dir} (a game built without TM-CE)')
    (folder / 'TM').mkdir()
    for path in training:
        shutil.copy2(path, folder / 'TM' / path.name)

    # lib/: the bundled libraries and their license files.
    lib = folder / 'lib'
    lib.mkdir()
    # bin/: the shader tools the OpenGL renderer runs (renderer_gl.cpp looks in bin/ first). Without
    # them the game stopped on its first frame ("Cannot run spirv-cross") on systems lacking them.
    tools_dir = folder / 'bin'
    tools_dir.mkdir()
    shader_tools = []
    for tool in ('glslangValidator', 'spirv-cross'):
        found = shutil.which(tool)
        if not found:
            raise SystemExit(f'missing {tool} on the build machine (apt install glslang-tools spirv-cross)')
        shutil.copy2(os.path.realpath(found), tools_dir / tool)
        os.chmod(tools_dir / tool, 0o755)
        shader_tools.append(tools_dir / tool)
    # DXC (libdxcompiler.so, the same compiler Windows uses): the Vulkan renderer compiles its shaders
    # with it from bin/ (gx_dxc_linux.cpp), with glslangValidator as the fallback.
    dxc = ROOT / 'port/third_party/dxc/linux/libdxcompiler.so'
    if not dxc.is_file():
        raise SystemExit(f'missing {dxc}: run python3 tools/fetch_graphics_dlls.py --dxc-linux')
    shutil.copy2(dxc, tools_dir / 'libdxcompiler.so')
    os.chmod(tools_dir / 'libdxcompiler.so', 0o755)
    shader_tools.append(tools_dir / 'libdxcompiler.so')
    # Mod import tools: the system's 7-Zip and xdelta3, as the Windows release carries its own. Before
    # the libraries are gathered, so theirs come along and resolve inside lib/ like the game's.
    tools = folder / 'tools'
    tools.mkdir()
    mod_tools = []
    for wanted, names in (('7zr', ('7zz', '7zr', '7za', '7z')), ('xdelta3', ('xdelta3',))):
        found = next((shutil.which(n) for n in names if shutil.which(n)), None)
        if found and open(found, 'rb').read(2) == b'#!':
            # Debian's 7zip installs 7zr as a two-line sh wrapper around /usr/lib/7zip/7zr: it only works where that
            # path exists, so a wrapper is never shipped (build 98 on Actions carried one). Ship the program it runs.
            target = re.search(r'exec\s+(\S+)', open(found, encoding='utf-8', errors='replace').read())
            real = target.group(1) if target else ''
            if real and os.path.isfile(real) and open(real, 'rb').read(4) == b'\x7fELF':
                print(f'{wanted}: {found} is a wrapper; bundling the program it runs, {real}')
                found = real
            else:
                print(f'warning: {found} is a script wrapper, not a program; {wanted} not bundled')
                found = None
        if found:
            shutil.copy2(found, tools / wanted)
            os.chmod(tools / wanted, 0o755)
            mod_tools.append((tools / wanted, os.path.realpath(found)))
        else:
            print(f'warning: {wanted} not found on this system; mod archives will need it installed')
    libraries = {}
    for binary in (exe, game, *((compat_exe, compat_game) if compat else ()), *shader_tools, *(t for t, _ in mod_tools)):
        libraries.update(ldd(binary))
    licenses = folder / 'licenses'
    licenses.mkdir()

    def owning_package(path):
        owner = subprocess.run(['dpkg', '-S', os.path.realpath(path)], capture_output=True, text=True).stdout
        if not owner:
            owner = subprocess.run(['dpkg', '-S', path], capture_output=True, text=True).stdout
        return owner.split(':')[0].strip() if owner else None

    packages = set()
    for soname, path in sorted(libraries.items()):
        if not bundled(soname):
            continue
        shutil.copy2(os.path.realpath(path), lib / soname)
        os.chmod(lib / soname, 0o755)
        if owning_package(path):
            packages.add(owning_package(path))
    # The programs in bin/ and tools/ that come from the build machine's packages: their copyright files too.
    for tool in ('glslangValidator', 'spirv-cross'):
        if owning_package(shutil.which(tool)):
            packages.add(owning_package(shutil.which(tool)))
    for _, source in mod_tools:
        if owning_package(source):
            packages.add(owning_package(source))
    for dxc_licence in ('LICENSE-LLVM.txt', 'LICENSE-MS.txt'):
        shutil.copy2(ROOT / 'port/third_party/dxc' / dxc_licence, licenses / f'directx-shader-compiler-{dxc_licence[8:]}')
    for package in sorted(packages):
        copyright_file = Path('/usr/share/doc') / package / 'copyright'
        if copyright_file.is_file():
            shutil.copyfile(copyright_file, licenses / f'lib-{package}.txt')
            os.chmod(licenses / f'lib-{package}.txt', 0o644)   # a build host's doc files can be 0777 (GitHub Actions runner)
    # librashader (shader presets), loaded at run time with dlopen, so ldd does not list it. Built from
    # its source tag (port/third_party/librashader/README.txt); without it the presets are off.
    rashader = ROOT / 'port/third_party/librashader/bin/librashader.so'
    if rashader.is_file():
        shutil.copy2(rashader, lib / 'librashader.so')
        os.chmod(lib / 'librashader.so', 0o755)
    else:
        print('lib: librashader.so not built; shader presets will be off on Linux')
    print(f'lib: {len(list(lib.iterdir()))} libraries from {len(packages)} packages')
    # A program's RUNPATH only serves its own direct dependencies, so each bundled library gets its own
    # ($ORIGIN = lib/): otherwise libcurl looked for librtmp and libssl in the system's folders, which
    # failed on SteamOS (librtmp missing, a newer libssl than the bundled libcrypto).
    for path in lib.iterdir():
        run('patchelf', '--set-rpath', '$ORIGIN', path)
    run('patchelf', '--set-rpath', '$ORIGIN/lib', folder / 'melee_game.so')
    if compat:
        run('patchelf', '--set-rpath', '$ORIGIN/lib', folder / 'melee_game_compat.so')
    # Last step that changes either game library: bind the snapshot ranges to the shipped size now.
    package_snapexcl(snapexcl, folder / 'melee_game.so', game)
    if compat:
        package_snapexcl(compat_snapexcl, folder / 'melee_game_compat.so', compat_game)
    for tool in [*shader_tools, *(t for t, _ in mod_tools)]:
        run('patchelf', '--set-rpath', '$ORIGIN/../lib', tool)
    # Prove it: with no help from the environment, every dependency the package bundles resolves to lib/.
    env = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
    wrong = []
    programs = [folder / EXE, folder / 'melee_game.so'] + \
        ([folder / (EXE + '_compat'), folder / 'melee_game_compat.so'] if compat else [])
    for binary in [*programs, *shader_tools, *(t for t, _ in mod_tools), *lib.iterdir()]:
        out = subprocess.run(['ldd', str(binary)], capture_output=True, text=True, env=env).stdout
        dynamic = subprocess.run(['readelf', '-d', str(binary)], capture_output=True, text=True).stdout
        needed = {line.split('[', 1)[1].split(']', 1)[0] for line in dynamic.splitlines() if '(NEEDED)' in line}
        # Each binary's own dependencies (what its RUNPATH serves); libraries only a system library
        # pulls in (the sound server's codecs) come with that system library.
        for soname, where in ldd_text(out).items():
            if soname not in needed:
                continue
            if (lib / soname).exists() and Path(where).resolve() != (lib / soname).resolve():
                wrong.append(f'{binary.name}: {soname} -> {where}')
            if where == 'not found':
                wrong.append(f'{binary.name}: {soname} not found')
    if wrong:
        raise SystemExit('bundled libraries do not resolve inside lib/:\n  ' + '\n  '.join(wrong[:20]))
    print('lib: every bundled dependency resolves inside lib/')

    # The game files, as in the Windows release.
    sys_src = ROOT / 'port/slippi_sys'
    sys_dst = folder / 'Sys'
    (sys_dst / 'GameSettings').mkdir(parents=True)
    shutil.copy2(sys_src / 'GameSettings/GALE01r2.ini', sys_dst / 'GameSettings/GALE01r2.ini')
    shutil.copy2(sys_src / 'codehandler.bin', sys_dst / 'codehandler.bin')
    shutil.copy2(sys_src / 'bootloader.gct', sys_dst / 'bootloader.gct')
    shutil.copytree(sys_src / 'GameFiles', sys_dst / 'GameFiles')
    coef = sys_src / 'GC/dsp_coef.bin'
    if hashlib.sha256(coef.read_bytes()).hexdigest() != 'c41e7d9f763da40983e83a5291cbecec1f536fd8b08ac6dda05eab33f57501ea':
        raise SystemExit(f'missing or unrecognized free DSP coefficient table: {coef}')
    (sys_dst / 'GC').mkdir()
    shutil.copy2(coef, sys_dst / 'GC/dsp_coef.bin')
    playback = ROOT / 'port/slippi_sys_playback'
    if (playback / 'codehandler.bin').is_file():
        shutil.copytree(playback, folder / 'SysPlayback', ignore=shutil.ignore_patterns('README.md', '.git*'))
        (folder / 'SysPlayback/GC').mkdir(exist_ok=True)
        shutil.copy2(coef, folder / 'SysPlayback/GC/dsp_coef.bin')
    # Two of Slippi's Sys files are mostly Nintendo's own disc assets (compliance review, 10-25): they never ship. The
    # host rebuilds them from the player's disc (host/sysgen.cpp reads deltas/ beside the program), as on the Mac.
    deltas = json.loads((ROOT / 'port/sys_deltas/manifest.json').read_text())
    for target in {f['target'] for f in deltas['files']}:
        for sys_folder in ('Sys', 'SysPlayback'):
            (folder / sys_folder / 'GameFiles/GALE01' / target).unlink(missing_ok=True)
    (folder / 'deltas').mkdir()
    shutil.copy2(ROOT / 'port/sys_deltas/manifest.json', folder / 'deltas/manifest.json')
    for f in deltas['files']:
        shutil.copy2(ROOT / 'port/sys_deltas' / f['delta'], folder / 'deltas' / f['delta'])
    (folder / 'Shaders').mkdir()
    (folder / 'Shaders/crt-shaders.pak').write_bytes(shader_pak())
    ui_src = ROOT / 'port/runtime/gx/ui_sources'
    for source in ('gd_melee', 'mockups', 'menu_previews', 'vendor'):
        shutil.copytree(ui_src / source / 'assets', folder / 'ui_sources' / source)
    for sub in ('User/Slippi', 'Replays', 'MusicPacks', 'Borders', 'VideoBackgrounds', 'Mods/Discs', 'Mods/Saves'):
        (folder / sub).mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'docs/training-mods.md', folder / 'TrainingMods.md')
    shutil.copy2(UDEV, folder / UDEV.name)
    # The launcher, with Unix line ends whatever the checkout made of them (a Windows working tree).
    (folder / 'Nucleus.sh').write_bytes(LAUNCHER.read_bytes().replace(b'\r\n', b'\n'))
    os.chmod(folder / 'Nucleus.sh', 0o755)
    # The oldest glibc every program and library here runs on, for the README.
    glibc = min_glibc([p for p in folder.rglob('*') if p.is_file() and p.read_bytes()[:4] == b'\x7fELF'])
    ubuntu = next((name for major, minor, name in reversed(UBUNTU_GLIBC) if (major, minor) >= glibc), None)
    print(f'glibc: {glibc[0]}.{glibc[1]} or newer (Ubuntu {ubuntu or "?"})')
    (folder / 'README.txt').write_text(README.format(
        version=version, underline='=' * len(f'Nucleus Unlocked {version} for Linux'),
        glibc=f'{glibc[0]}.{glibc[1]}', ubuntu=ubuntu or 'of the same age',
        # glibc 2.35 / 2.36: what the distributions players still often run have.
        too_old='' if glibc <= (2, 35) else
        '\n    Ubuntu 22.04, Linux Mint 21 and Debian 12 are too old.' if glibc > (2, 36) else
        '\n    Ubuntu 22.04 and Linux Mint 21 are too old.',
        cpu=CPU_COMPAT if compat else CPU_AVX2), encoding='utf-8')
    for src, dst in ((ROOT / 'LICENSE', 'COPYING.txt'), (ROOT / 'NOTICE', 'NOTICE.txt'),
                     (ROOT / 'port/third_party/monocypher/LICENCE.md', 'monocypher.md'),
                     (ROOT / 'port/third_party/enet/LICENSE', 'enet.txt'),
                     (ROOT / 'port/third_party/imgui/LICENSE.txt', 'imgui.txt'),
                     (ROOT / 'port/third_party/stb/LICENSE', 'stb.txt'),
                     (ROOT / 'port/third_party/zstd/LICENSE', 'zstd.txt'),
                     (ROOT / 'port/third_party/earcut/LICENSE', 'earcut.txt'),
                     (ROOT / 'port/third_party/librashader/LICENSE.md', 'librashader-MPL-2.0.md'),
                     (ROOT / 'docs/licenses/dsp-coefficients.txt', 'dsp-coefficients.txt'),
                     (ROOT / 'docs/licenses/hps_decode-MIT.txt', 'hps_decode.txt'),
                     (ROOT / 'port/runtime/gx/ui_sources/gd_melee/assets/kit/SourceSans3-OFL.md', 'source-sans-3-OFL.md'),
                     (ROOT / 'docs/third-party-ui-attribution.md', 'ui-attribution.md'),
                     (ROOT / 'docs/third-party-training-mode-ce.txt', 'training-mode-ce.txt'),
                     # The same notices as the Windows client (windows-build/client_release.py LICENSES).
                     (ROOT / 'port/third_party/libusb/COPYING', 'libusb-LGPL-2.1.txt'),
                     (ROOT / 'port/runtime/gx/ui_sources/gd_melee/ORIGIN.md', 'gd-melee-origin.md'),
                     (ROOT / 'port/third_party/mod_tools/xdelta-COPYING.txt', 'xdelta3-GPL-2.txt'),
                     (ROOT / 'port/third_party/mod_tools/7zip-license.txt', '7zip.txt')):
        if not src.is_file():
            raise SystemExit(f'missing license text: {src}')
        shutil.copy2(src, licenses / dst)
    sys.path.insert(0, str(ROOT / 'tools'))
    import package_release   # the notices generated from the sources, as the Windows client writes them
    (licenses / 'nlohmann-json.txt').write_text(package_release.json_notice(), encoding='utf-8')
    (licenses / 'imgui-fonts.txt').write_text(package_release.imgui_font_notice(), encoding='utf-8')
    ship_vulkan_cache(ROOT / 'shadercache/vulkan-v1', folder / 'shadercache/vulkan-v1')
    for path in folder.rglob('*'):
        if path.name.endswith('.md') and path.parent == folder and path.name != 'TrainingMods.md':
            raise SystemExit(f'private notes in the package: {path}')

    archive = args.out / f'{name}.tar.gz'
    if archive.exists():
        archive.unlink()

    def entry(item):
        # The crash symbols stay in the unpacked folder (addr2line reads them there); the game never
        # opens them. No build machine's user or group names in the archive.
        if item.name.endswith('.dbg'):
            return None
        item.uid = item.gid = 0
        item.uname = item.gname = ''
        return item

    with tarfile.open(archive, 'w:gz', compresslevel=9) as tar:
        tar.add(folder, arcname=name, filter=entry)
    symbols = args.out / 'symbols' / name
    symbols.mkdir(parents=True, exist_ok=True)
    shutil.copy2(folder / 'melee_game.dbg', symbols / 'melee_game.dbg')   # kept for crash reports (bugwatch release_check reads it)
    total = sum(p.stat().st_size for p in folder.rglob('*') if p.is_file() and not p.name.endswith('.dbg'))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f'{archive} ({archive.stat().st_size / 1e6:.1f} MB packed, {total / 1e6:.1f} MB unpacked), sha256 {digest}')


if __name__ == '__main__':
    main()
