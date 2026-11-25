"""The Source Port client release: one folder and its zip holding melee_source.exe, melee_game.dll and
what they need at run time (no launcher, no Static Recomp), and a manifest of every file in it. Also
checks a folder against a manifest (--verify), which is what SSBM Nucleus runs after a download.

    python windows-build/client_release.py --host-build build-host --game-build build-game [--out release/client]
    python windows-build/client_release.py --host-dir <folder with melee_source.exe> --game-dir <folder with melee_game.dll> [...]
    python windows-build/client_release.py --verify <folder> [--manifest <file>] [--strict]

Two toolchains build the client (docs/client-release.md, docs/build-source-port.md):

    melee_source.exe  MSVC 2022, CMake's "Visual Studio 17 2022" generator, x64, from the repository root
                      (its outputs in <host build>/port/Release)
    melee_game.dll    MinGW-w64 GCC 14+ with Ninja, from sourceport/game (melee_game.dll, .snapexcl and
                      .syms in <game build>)

With --host-build/--game-build the script builds both targets first (cmake --build, below-normal
priority, --jobs at a time); with --host-dir/--game-dir it takes outputs built elsewhere. The pinned
graphics libraries come from tools/fetch_graphics_dlls.py, which checks their SHA-256 (and downloads
them only when they are not already in place).

Both must be built with -DNUCLEUS_PUBLIC_BUILD=ON (docs/client-release.md): the packaged program's
--print-info must say "public_build": true and "game_public_build": true, or nothing is packaged.

Output, in --out:
    NucleusClient-<version>-win64/                 the release folder, with release-manifest.json
    NucleusClient-<version>-win64.zip              the same folder zipped
    NucleusClient-<version>-win64.manifest.json    the manifest again, with the zip's size and SHA-256
    NucleusClient-<version>-win64-symbols/         melee_game.dbg when the game build has one (not shipped)
    NucleusClient-<version>-win64-source/ and .zip with --source: the corresponding source as it is
                                                   published (tools/decomp_fork.py public-snapshot of the
                                                   commit: public decompilation URL and pin plus the native
                                                   patch generated from the fork); the manifest records it

The manifest (kind "nucleus-client"): version, git commit, the packaged program's --print-info answer, the fighter plugin format
and ABI it takes, and each file's path, size and SHA-256. Nothing here signs, uploads or publishes:
a signature changes the files, so any signing has to happen before the manifest is written, by hand.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fetch_graphics_dlls  # noqa: E402  pinned graphics libraries
import package_release  # noqa: E402  the generated license notices

MANIFEST_NAME = "release-manifest.json"
MANIFEST_FORMAT = 1
KIND = "nucleus-client"
NAME = "NucleusClient-{version}-win64"   # the folder, and the stem of the zip and manifest beside it

GAME_FILES = ("melee_game.dll", "melee_game.snapexcl", "melee_game.syms")   # .syms: fighter plugins link against it
DLSS_DLLS = ("sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll", "sl.dlss_g.dll",
             "nvngx_dlssg.dll", "sl.reflex.dll", "sl.pcl.dll")
GRAPHICS_KINDS = ("dxc", "librashader")
MOD_TOOLS = (("xdelta3.exe", "d81f59b2fe5e8589c0ee9782e231c805084f4d23dfade413903a4cad63b4e342"),
             ("7zr.exe", "ad4c82fadcbdf93c03b4fc440f300509c7d60c5c2f4d183e35d9d70d6957037d"))
DSP_COEF_SHA256 = "c41e7d9f763da40983e83a5291cbecec1f536fd8b08ac6dda05eab33f57501ea"
# The same licence texts as tools/package_release.py ships (client_release_test.py keeps the lists equal).
LICENSES = (
    ("LICENSE", "COPYING.txt"),
    ("NOTICE", "NOTICE.txt"),
    ("port/third_party/streamline/license.txt", "streamline.txt"),
    ("port/third_party/streamline/3rd-party-licenses.md", "streamline-third-party.md"),
    ("port/third_party/streamline/reflex.license.txt", "nvidia-reflex.txt"),
    ("port/third_party/ngx/LICENSE.txt", "nvidia-rtx-sdks.txt"),
    ("docs/licenses/nvidia-dlss-third-party.txt", "nvidia-dlss-third-party.txt"),
    ("port/third_party/fsr/LICENSE.md", "amd-fsr.md"),
    ("port/third_party/librashader/LICENSE.md", "librashader-MPL-2.0.md"),
    ("port/third_party/dxc/LICENSE-LLVM.txt", "directx-shader-compiler-LLVM.txt"),
    ("port/third_party/dxc/LICENSE-MS.txt", "directx-shader-compiler-dxil-Microsoft.txt"),
    ("port/third_party/monocypher/LICENCE.md", "monocypher.md"),
    ("port/third_party/enet/LICENSE", "enet.txt"),
    ("port/third_party/imgui/LICENSE.txt", "imgui.txt"),
    ("port/third_party/libusb/COPYING", "libusb-LGPL-2.1.txt"),
    ("port/third_party/stb/LICENSE", "stb.txt"),
    ("port/third_party/zstd/LICENSE", "zstd.txt"),
    ("port/third_party/earcut/LICENSE", "earcut.txt"),
    ("port/third_party/asio/LICENSE.txt", "steinberg-asio-sdk.txt"),
    ("port/third_party/asio/NOTICE-BSD.txt", "steinberg-asio-bsd.txt"),
    ("docs/licenses/asio.html", "asio.html"),
    ("docs/licenses/asio-compatible-logo-white.svg", "asio-compatible-logo-white.svg"),
    ("docs/licenses/dsp-coefficients.txt", "dsp-coefficients.txt"),
    ("docs/licenses/hps_decode-MIT.txt", "hps_decode.txt"),
    ("port/runtime/gx/ui_sources/gd_melee/assets/kit/SourceSans3-OFL.md", "source-sans-3-OFL.md"),
)

README = """Nucleus Unlocked {version}
============================

Super Smash Bros. Melee NTSC 1.02 as a native Windows program, compiled from the game's C source
(melee_source.exe with melee_game.dll). You need your own Melee NTSC 1.02 ISO; nothing from the game
is included. Not affiliated with, endorsed by, or supported by Nintendo, HAL Laboratory or the Slippi
team.

Start: from the SSBM Nucleus app, or double-click melee_source.exe. The first time, it asks for your ISO
(or takes melee.iso from this folder) and remembers it. melee_source.exe --help lists the command line
for a launcher or a script.

Replays of a double-click start go where the Slippi Launcher keeps its replays (its replay folder, else
Documents\\Slippi, in month folders when the Launcher's monthly subfolders are on), so replay trackers
find them; with neither folder, into %LOCALAPPDATA%\\Nucleus Client\\Replays.

Escape closes the game. The settings are not in the game window: change them in the SSBM Nucleus app or in
the separate settings window (melee_source.exe --settings-window).

Fighter plugins (.mufighter files) are program code. One loads only when SSBM Nucleus signed it or you
approved that exact file; melee_source.exe --list-plugins lists them.

release-manifest.json lists every file of this release with its size and SHA-256.

Nucleus Unlocked is built from Melee Unlocked.
Melee Unlocked is free software under the GNU General Public License, version 3 or (at your option)
any later version: licenses\\COPYING.txt, with the notices in licenses\\NOTICE.txt. The NVIDIA, Microsoft
and other runtime files in this folder are not covered by the GPL; their own terms are in licenses\\.
"""


class ReleaseError(SystemExit):
    pass


# ---- small helpers ----
def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def copy(source: Path, target: Path) -> None:
    if not source.is_file():
        raise ReleaseError(f"missing {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def below_normal() -> dict:
    """Builds and probes run at below-normal priority (a shared machine stays usable)."""
    return {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def find_cmake() -> str:
    found = shutil.which("cmake")
    if found:
        return found
    bundled = sorted(Path(r"C:\Program Files\Microsoft Visual Studio\2022").glob(
        "*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"))
    return str(bundled[-1]) if bundled else "cmake"


def compiler_of(build_dir: Path | None, language: str) -> str | None:
    """"MSVC 19.42.34435.0" / "GNU 14.2.0": what CMake found when it configured `build_dir`."""
    if not build_dir:
        return None
    for path in sorted(build_dir.glob(f"CMakeFiles/*/CMake{language}Compiler.cmake"), reverse=True):
        text = path.read_text(encoding="utf-8", errors="replace")
        ident = re.search(rf'set\(CMAKE_{language}_COMPILER_ID "([^"]*)"\)', text)
        version = re.search(rf'set\(CMAKE_{language}_COMPILER_VERSION "([^"]*)"\)', text)
        if ident:
            return f"{ident.group(1)} {version.group(1) if version else '?'}".strip()
    return None


def safe_relative(path: str) -> bool:
    """A manifest path that stays inside the folder: relative, forward slashes, no '..' or drive."""
    p = PurePosixPath(path)
    return bool(path) and "\\" not in path and ":" not in path and not p.is_absolute() and ".." not in p.parts


def folder_files(folder: Path) -> list[str]:
    return sorted(p.relative_to(folder).as_posix() for p in folder.rglob("*")
                  if p.is_file() and p.relative_to(folder).as_posix() != MANIFEST_NAME)


def file_entries(folder: Path) -> list[dict]:
    return [{"path": rel, "size": (folder / rel).stat().st_size, "sha256": sha256_file(folder / rel)}
            for rel in folder_files(folder)]


# ---- building ----
def build(cmake: str, build_dir: Path, target: str, config: str | None, jobs: int) -> None:
    command = [cmake, "--build", str(build_dir), "--target", target, "--parallel", str(jobs)]
    if config:
        command[3:3] = ["--config", config]
    print("build:", " ".join(command), flush=True)
    subprocess.run(command, check=True, **below_normal())


def fetch_graphics() -> dict[str, Path]:
    """tools/fetch_graphics_dlls.py for the libraries shipped in the folder; returns name -> checked file."""
    subprocess.run([sys.executable, str(ROOT / "tools/fetch_graphics_dlls.py")] + [f"--{k}" for k in GRAPHICS_KINDS],
                   check=True)
    out = {}
    for kind in GRAPHICS_KINDS:
        for _member, dest, pinned in fetch_graphics_dlls.PACKAGES[kind][2]:
            path = fetch_graphics_dlls.THIRD_PARTY / dest
            if not path.is_file() or sha256_file(path) != pinned:
                raise ReleaseError(f"{path} is not the pinned {kind} library")
            out[Path(dest).name] = path
    return out


# ---- the folder ----
def shader_pak() -> bytes:
    """The CRT shader presets as one file (port/runtime/gx/user_shader.cpp reads this layout), as
    tools/package_release.py makes it."""
    shaders = ROOT / "port/third_party/slang-shaders"
    if not (shaders / "crt").is_dir():
        raise ReleaseError(f"missing shader presets: {shaders}")
    entries = sorted((p.relative_to(shaders).as_posix(), p.read_bytes()) for p in shaders.rglob("*") if p.is_file())
    digest = hashlib.sha256()
    for entry, data in entries:
        if ".." in entry or ":" in entry or "\\" in entry or len(entry.encode("utf-8")) >= 400:
            raise ReleaseError(f"shader preset file name the game would refuse: {entry}")
        digest.update(entry.encode("utf-8") + b"\0" + data)
    pak = bytearray(b"MUSHPAK1" + digest.hexdigest()[:16].encode("ascii") + struct.pack("<I", len(entries)))
    for entry, data in entries:
        raw = entry.encode("utf-8")
        pak += struct.pack("<H", len(raw)) + raw + struct.pack("<I", len(data)) + data
    return bytes(pak)


def copy_sys(folder: Path) -> None:
    """Sys and SysPlayback as the full release has them, without the menu files the game rebuilds from
    the player's disc (deltas/)."""
    sys_src = ROOT / "port/slippi_sys"
    sys_dst = folder / "Sys"
    copy(sys_src / "GameSettings/GALE01r2.ini", sys_dst / "GameSettings/GALE01r2.ini")
    copy(sys_src / "codehandler.bin", sys_dst / "codehandler.bin")
    copy(sys_src / "bootloader.gct", sys_dst / "bootloader.gct")
    shutil.copytree(sys_src / "GameFiles", sys_dst / "GameFiles")
    coef = sys_src / "GC/dsp_coef.bin"
    if not coef.is_file() or sha256_file(coef) != DSP_COEF_SHA256:
        raise ReleaseError(f"missing or unrecognized free DSP coefficient table: {coef}")
    copy(coef, sys_dst / "GC/dsp_coef.bin")
    playback = ROOT / "port/slippi_sys_playback"
    if not (playback / "codehandler.bin").is_file():
        raise ReleaseError(f"missing playback Sys folder: {playback}")
    shutil.copytree(playback, folder / "SysPlayback", ignore=shutil.ignore_patterns("README.md", ".git*"))
    copy(coef, folder / "SysPlayback/GC/dsp_coef.bin")
    deltas = json.loads((ROOT / "port/sys_deltas/manifest.json").read_text(encoding="utf-8"))
    for target in {f["target"] for f in deltas["files"]}:
        for sys_folder in ("Sys", "SysPlayback"):
            (folder / sys_folder / "GameFiles/GALE01" / target).unlink(missing_ok=True)
    copy(ROOT / "port/sys_deltas/manifest.json", folder / "deltas/manifest.json")
    for f in deltas["files"]:
        copy(ROOT / "port/sys_deltas" / f["delta"], folder / "deltas" / f["delta"])


def vc_runtime(override: Path | None) -> Path:
    if override:
        return override
    found = []
    for base in (r"C:\Program Files (x86)\Microsoft Visual Studio\2022", r"C:\Program Files\Microsoft Visual Studio\2022"):
        found += sorted(Path(base).glob("*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT"))
    if not found:
        raise ReleaseError("Visual C++ runtime redistributable not found (VC\\Redist\\MSVC\\*\\x64\\Microsoft.VC143.CRT); "
                           "give --vc-runtime")
    return found[-1]


def assemble(folder: Path, host_dir: Path, game_dir: Path, graphics: dict[str, Path], version: str,
             vc_dir: Path, require_dlss: bool) -> list[str]:
    """Fills `folder` (made new); returns notes for the manifest about what optional parts are in it."""
    notes = []
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    copy(host_dir / "melee_source.exe", folder / "melee_source.exe")
    for name in GAME_FILES:
        copy(game_dir / name, folder / name)
    # Training Mode CE's data files, written by the game's build into TM/ beside the library (upstream's
    # dats/ at the pinned commit, hash-checked): Event Match opens TM-CE with them.
    training = sorted((game_dir / "TM").glob("*.dat"))
    for path in training:
        copy(path, folder / "TM" / path.name)
    if not training:
        notes.append("no TM/*.dat beside melee_game.dll (a game built without TM-CE): Event Match is the retail events")
    for name, path in graphics.items():
        copy(path, folder / name)
    present = [n for n in DLSS_DLLS if (host_dir / n).is_file()]
    if present and len(present) != len(DLSS_DLLS):
        raise ReleaseError(f"incomplete DLSS runtime beside melee_source.exe: missing {sorted(set(DLSS_DLLS) - set(present))}")
    if present:
        for name in DLSS_DLLS:
            copy(host_dir / name, folder / name)
    elif require_dlss:
        raise ReleaseError(f"no DLSS runtime (Streamline) beside {host_dir / 'melee_source.exe'} and --require-dlss was given")
    else:
        notes.append("no DLSS runtime: the host was built without Streamline")
    dlls = sorted(vc_dir.glob("*.dll"))
    if not dlls:
        raise ReleaseError(f"no Visual C++ runtime DLLs in {vc_dir}")
    for dll in dlls:
        copy(dll, folder / dll.name)
    copy_sys(folder)
    (folder / "Shaders").mkdir()
    (folder / "Shaders/crt-shaders.pak").write_bytes(shader_pak())
    recipes = ROOT / "shadercache/recipes.bin"
    if recipes.is_file():
        copy(recipes, folder / "shadercache/recipes.shipped.bin")
    else:
        notes.append("no shipped pipeline recipes (shadercache/recipes.bin): the first start compiles them")
    ui_src = ROOT / "port/runtime/gx/ui_sources"
    for source in ("gd_melee", "mockups", "menu_previews", "vendor"):
        if not (ui_src / source / "assets").is_dir():
            raise ReleaseError(f"missing settings appearance assets: {ui_src / source / 'assets'}")
        shutil.copytree(ui_src / source / "assets", folder / "ui_sources" / source)
    licenses = folder / "licenses"
    for source, target in LICENSES:
        copy(ROOT / source, licenses / target)
    mod_tools = ROOT / "port/third_party/mod_tools"
    for source, target in (("xdelta-COPYING.txt", "xdelta3-GPL-2.txt"), ("7zip-license.txt", "7zip.txt"),
                           ("SOURCES.txt", "mod-tools-sources.txt")):
        copy(mod_tools / source, licenses / target)
    (licenses / "nlohmann-json.txt").write_text(package_release.json_notice(), encoding="utf-8")
    (licenses / "imgui-fonts.txt").write_text(package_release.imgui_font_notice(), encoding="utf-8")
    copy(ROOT / "docs/third-party-ui-attribution.md", licenses / "ui-attribution.md")
    copy(ROOT / "docs/third-party-training-mode-ce.txt", licenses / "training-mode-ce.txt")
    copy(ui_src / "gd_melee/ORIGIN.md", licenses / "gd-melee-origin.md")
    missing_tools = []
    for tool, digest in MOD_TOOLS:
        source = mod_tools / tool
        if source.is_file():
            if sha256_file(source) != digest:
                raise ReleaseError(f"altered mod tool: {source}")
            copy(source, folder / "tools" / tool)
        else:
            missing_tools.append(tool)
    if missing_tools:
        notes.append(f"mod tools not included ({', '.join(missing_tools)} not in port/third_party/mod_tools): "
                     "xdelta and 7z mods cannot be applied")
    copy(ROOT / "docs/training-mods.md", folder / "TrainingMods.md")
    # No batch file: a double-click on melee_source.exe finds the disc itself (asking once) and starts
    # the game (port/app/standalone_launch_win.h); the old MeleeUnlocked.bat fell back to a program
    # this release does not have.
    (folder / "README.txt").write_text(README.format(version=version), encoding="utf-8", newline="\r\n")
    return notes


# ---- the program's own answer ----
def run_program(exe: Path, args: list[str], cwd: Path) -> tuple[int, str]:
    done = subprocess.run([str(exe), *args], cwd=cwd, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                          timeout=60, **below_normal())
    return done.returncode, done.stdout


def scrub(value, folder: Path):
    """The packaged program's answer without this machine's path to the release folder."""
    if isinstance(value, dict):
        return {k: scrub(v, folder) for k, v in value.items()}
    if isinstance(value, list):
        return [scrub(v, folder) for v in value]
    if isinstance(value, str):
        for form in {str(folder), str(folder).replace("\\", "/")}:
            value = re.sub(re.escape(form), "<release>", value, flags=re.IGNORECASE)
    return value


def probe(folder: Path, version: str, runner=run_program) -> dict:
    exe = folder / "melee_source.exe"
    before = folder_files(folder)
    code, out = runner(exe, ["--version"], folder)
    if code != 0 or out.strip() != version:
        raise ReleaseError(f"melee_source.exe --version answered {out.strip()!r} (exit {code}), the release is {version!r}")
    code, out = runner(exe, ["--print-info"], folder)
    try:
        info = json.loads(out)
    except json.JSONDecodeError as error:
        raise ReleaseError(f"melee_source.exe --print-info did not answer JSON (exit {code}): {out[:200]!r}") from error
    if code != 0 or "error" in info:
        raise ReleaseError(f"melee_source.exe --print-info failed (exit {code}): {out[:300]}")
    if folder_files(folder) != before:
        raise ReleaseError("melee_source.exe --print-info wrote files into the release folder")
    if info.get("plugin_test_key"):
        raise ReleaseError("this melee_source.exe trusts the TEST-ONLY plugin signing key (-DMELEE_PLUGIN_TEST_KEY=ON); "
                           "rebuild it without that option")
    if "plugin_signing_keys" not in info:
        raise ReleaseError("this melee_source.exe has no fighter plugin approval (plugin_signing_keys missing from --print-info)")
    if info.get("engine") != "source":
        raise ReleaseError(f"--print-info says engine {info.get('engine')!r}, not the Source Port")
    if not info.get("plugin_abi") or info.get("plugin_format") is None:
        raise ReleaseError("--print-info names no fighter plugin ABI: melee_game.dll or melee_game.syms is not usable")
    # Players get only the public build (port/runtime/host/public_build.h): no test fixture's entry point,
    # no test hook read from the environment, other players hashed in the log.
    if info.get("public_build") is not True:
        raise ReleaseError("this melee_source.exe is not a public build (--print-info public_build is not true); "
                           "configure the host with -DNUCLEUS_PUBLIC_BUILD=ON and build it again")
    if info.get("game_public_build") is not True:
        raise ReleaseError("this melee_game.dll is not a public build (--print-info game_public_build is not true); "
                           "configure sourceport/game with -DNUCLEUS_PUBLIC_BUILD=ON and build it again")
    return scrub(info, folder)


# ---- the manifest and the zip ----
def write_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")


def make_zip(folder: Path, archive: Path, when: tuple) -> None:
    part = archive.with_name(archive.name + ".part")
    with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for rel in folder_files(folder) + [MANIFEST_NAME]:
            info = zipfile.ZipInfo(f"{folder.name}/{rel}", date_time=when)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with (folder / rel).open("rb") as source, z.open(info, "w") as target:
                shutil.copyfileobj(source, target, 1 << 20)
    os.replace(part, archive)


def corresponding_source(out: Path, name: str, commit: str) -> dict:
    """The release's corresponding source as it is published: tools/decomp_fork.py public-snapshot of the
    commit the release is built from (the public decompilation's URL and pin plus the native patch made
    from the fork commit that commit records; nothing that names the private fork), zipped beside the
    release as <name>-source.zip."""
    import decomp_fork  # noqa: E402  (tools/ is on sys.path)
    folder = out / f"{name}-source"
    if folder.exists():
        shutil.rmtree(folder)
    try:
        info = decomp_fork.public_snapshot(folder, commit)
    except decomp_fork.Failure as error:
        raise ReleaseError(f"corresponding source: {error}") from error
    archive = out / f"{name}-source.zip"
    when = dt.datetime.fromisoformat(git("log", "-1", "--format=%cI", commit)).astimezone(dt.timezone.utc).timetuple()[:6]
    part = archive.with_name(archive.name + ".part")
    with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(p for p in folder.rglob("*") if p.is_file()):
            info_entry = zipfile.ZipInfo(f"{folder.name}/{path.relative_to(folder).as_posix()}", date_time=when)
            info_entry.compress_type = zipfile.ZIP_DEFLATED
            info_entry.external_attr = 0o644 << 16
            with path.open("rb") as source, z.open(info_entry, "w") as target:
                shutil.copyfileobj(source, target, 1 << 20)
    os.replace(part, archive)
    return {"zip": archive.name, "size": archive.stat().st_size, "sha256": sha256_file(archive),
            "decomp_url": decomp_fork.PUBLIC_URL, "decomp_commit": info["pin"],
            "native_patch_sha256": sha256_file(folder / decomp_fork.PATCH_PATH)}


def package(args, runner=run_program) -> dict:
    version = args.version
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    if dirty and not args.allow_dirty:
        raise ReleaseError("the working tree has uncommitted changes: commit them first (or --allow-dirty for a test package)")
    commit = git("rev-parse", "HEAD")
    commit_time = dt.datetime.fromisoformat(git("log", "-1", "--format=%cI")).astimezone(dt.timezone.utc)
    if args.host_build:
        if not args.no_build:
            build(args.cmake, args.host_build, "melee_source", "Release", args.jobs)
        host_dir = args.host_build / "port/Release"
    else:
        host_dir = args.host_dir
    if args.game_build:
        if not args.no_build:
            build(args.cmake, args.game_build, "melee_game", None, args.jobs)
        game_dir = args.game_build
    else:
        game_dir = args.game_dir
    if not host_dir or not game_dir:
        raise ReleaseError("give --host-build or --host-dir, and --game-build or --game-dir")
    graphics = fetch_graphics()
    name = NAME.format(version=version)
    out = args.out.resolve()
    folder = out / name
    notes = assemble(folder, host_dir.resolve(), game_dir.resolve(), graphics, version, vc_runtime(args.vc_runtime),
                     args.require_dlss)
    info = probe(folder, version, runner)
    symbols = game_dir / "melee_game.dbg"
    if symbols.is_file():
        copy(symbols, out / f"{name}-symbols" / "melee_game.dbg")
    source = corresponding_source(out, name, commit) if getattr(args, "source", False) else None
    manifest = {
        "format": MANIFEST_FORMAT,
        "kind": KIND,
        "name": name,
        "version": version,
        "git_commit": commit,
        "git_dirty": dirty,
        "created": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "toolchains": {
            "host": compiler_of(args.host_build, "CXX") or "prebuilt (--host-dir)",
            "game": compiler_of(args.game_build, "C") or "prebuilt (--game-dir)",
        },
        "plugin_format": info.get("plugin_format"),
        "plugin_abi": info.get("plugin_abi"),
        "notes": notes,
        "print_info": info,
        "files": file_entries(folder),
    }
    if source:
        manifest["corresponding_source"] = source
    write_json(folder / MANIFEST_NAME, manifest)
    archive = out / f"{name}.zip"
    make_zip(folder, archive, commit_time.timetuple()[:6])
    outer = dict(manifest, zip={"name": archive.name, "size": archive.stat().st_size, "sha256": sha256_file(archive)})
    write_json(out / f"{name}.manifest.json", outer)
    total = sum(f["size"] for f in manifest["files"])
    print(f"{folder}: {len(manifest['files'])} files, {total / 1e6:.1f} MB")
    print(f"{archive}: {archive.stat().st_size / 1e6:.1f} MB, sha256 {outer['zip']['sha256']}")
    print(f"{out / (name + '.manifest.json')}")
    if source:
        print(f"{out / source['zip']}: corresponding source (public decompilation {source['decomp_commit'][:12]} "
              f"plus the native patch), sha256 {source['sha256']}")
    for note in notes:
        print(f"note: {note}")
    return outer


# ---- checking a folder ----
def verify_folder(folder: Path, manifest_path: Path | None = None, strict: bool = False) -> tuple[int, dict]:
    """(exit code, report): 0 every listed file is there unchanged (and, with strict, nothing else is),
    1 something differs, 2 the manifest cannot be used."""
    folder = folder.resolve()
    manifest_path = (manifest_path or folder / MANIFEST_NAME).resolve()
    report = {"action": "verify", "ok": False, "folder": str(folder), "manifest": str(manifest_path)}
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("format") != MANIFEST_FORMAT or manifest.get("kind") != KIND:
            raise ValueError(f"not a format {MANIFEST_FORMAT} {KIND} manifest")
        files = manifest["files"]
        expected = {}
        for entry in files:
            path, size, sha = entry["path"], entry["size"], entry["sha256"]
            if not safe_relative(path) or path == MANIFEST_NAME or path in expected:
                raise ValueError(f"unusable file entry {path!r}")
            if not isinstance(size, int) or not re.fullmatch(r"[0-9a-f]{64}", str(sha)):
                raise ValueError(f"unusable size or sha256 for {path!r}")
            expected[path] = entry
    except (OSError, ValueError, KeyError, TypeError) as error:
        report["error"] = f"{manifest_path}: {error}"
        return 2, report
    if not folder.is_dir():
        report["error"] = f"{folder} is not a folder"
        return 2, report
    present = set(folder_files(folder))
    changed, missing = [], []
    for path, entry in sorted(expected.items()):
        if path not in present:
            missing.append(path)
            continue
        actual = folder / path
        size = actual.stat().st_size
        sha = sha256_file(actual) if size == entry["size"] else None
        if size != entry["size"] or sha != entry["sha256"]:
            changed.append({"path": path, "expected_size": entry["size"], "size": size,
                            "expected_sha256": entry["sha256"], "sha256": sha or sha256_file(actual)})
    extra = sorted(present - set(expected))
    report.update({"version": manifest.get("version"), "git_commit": manifest.get("git_commit"),
                   "checked": len(expected), "changed": changed, "missing": missing, "extra": extra,
                   "strict": strict})
    report["ok"] = not changed and not missing and not (strict and extra)
    return (0 if report["ok"] else 1), report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", type=Path, metavar="FOLDER", help="check FOLDER against its manifest and exit")
    ap.add_argument("--manifest", type=Path, help="with --verify: the manifest to check against "
                    "(default FOLDER/release-manifest.json; a manifest from a trusted source is the stronger check)")
    ap.add_argument("--strict", action="store_true", help="with --verify: files the manifest does not list fail too")
    ap.add_argument("--host-build", type=Path, help="CMake build folder of the host (MSVC); built unless --no-build")
    ap.add_argument("--game-build", type=Path, help="CMake build folder of sourceport/game (MinGW GCC); built unless --no-build")
    ap.add_argument("--host-dir", type=Path, help="instead of --host-build: a folder holding a built melee_source.exe")
    ap.add_argument("--game-dir", type=Path, help="instead of --game-build: a folder holding melee_game.dll, .snapexcl and .syms")
    ap.add_argument("--no-build", action="store_true", help="take --host-build/--game-build outputs as they are")
    ap.add_argument("--jobs", type=int, default=3, help="parallel compile jobs (default 3)")
    ap.add_argument("--cmake", default=None, help="cmake to build with (default: on PATH, else Visual Studio's)")
    ap.add_argument("--out", type=Path, default=ROOT / "release/client")
    ap.add_argument("--version", default=(ROOT / "VERSION").read_text(encoding="utf-8").strip())
    ap.add_argument("--vc-runtime", type=Path, help="folder of the Visual C++ runtime DLLs (default: Visual Studio's redist)")
    ap.add_argument("--require-dlss", action="store_true", help="fail when the host has no DLSS runtime beside it")
    ap.add_argument("--allow-dirty", action="store_true", help="package uncommitted changes (recorded as git_dirty)")
    ap.add_argument("--source", action="store_true",
                    help="also write the corresponding source, <name>-source.zip: the public snapshot of the commit "
                         "(tools/decomp_fork.py public-snapshot), recorded in the manifest")
    args = ap.parse_args(argv)
    if args.verify:
        code, report = verify_folder(args.verify, args.manifest, args.strict)
        print(json.dumps(report))
        return code
    args.cmake = args.cmake or find_cmake()
    try:
        package(args)
    except subprocess.CalledProcessError as error:
        print(f"failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
