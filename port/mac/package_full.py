"""Unsigned M3 app; package existing inputs only, never fetch/build game or dependencies."""
from pathlib import Path
from build_full import BUILD, GAME_LIBRARY, squad_enabled
import concurrent.futures
import argparse, hashlib, json, os, platform, plistlib, runpy, shutil, subprocess
ROOT = Path(__file__).resolve().parents[2]
APP = BUILD/'MeleeUnlocked.app'
# The ELF, exclusions, and Sys assets must come from the same native build.
GAME_ASSETS = GAME_LIBRARY.parent

def copy(src, dst):
    if not src.exists():
        raise SystemExit(f'Missing on-disk packaging input: {src}')
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, dst, symlinks=False, ignore=shutil.ignore_patterns('.git*', 'README.md'))
    else:
        shutil.copy2(src, dst, follow_symlinks=True)

def build_shader_seed(destination, tools):
    # Recipes are scale independent. Generated GLSL uses the target EFB height.
    builder = BUILD/'seed-build'
    subprocess.run(['/usr/bin/clang++', '-arch', 'x86_64', '-mmacosx-version-min=15.0',
                    '-std=c++17', '-O2', '-pthread', '-I'+str(ROOT/'port/runtime/gx'),
                    ROOT/'port/mac/seed_build.cpp', BUILD/'objects/port_runtime_gx_gx_shader.cpp.o',
                    BUILD/'objects/port_runtime_gx_gx_texture.cpp.o', '-o', builder], check=True)
    try:
        subprocess.run([builder], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        # Apple silicon without Rosetta cannot start the x86-64 builder ("Bad CPU type in executable"). The
        # generator is plain C++ whose output does not depend on the architecture: build it for this Mac.
        subprocess.run(['/usr/bin/clang++', '-arch', platform.machine(), '-mmacosx-version-min=15.0',
                        '-std=c++17', '-O2', '-pthread', '-I'+str(ROOT/'port/runtime/gx'),
                        ROOT/'port/mac/seed_build.cpp', ROOT/'port/runtime/gx/gx_shader.cpp',
                        ROOT/'port/runtime/gx/gx_texture.cpp', '-o', builder], check=True)

    def generate(scale):
        subprocess.run([builder, ROOT/'port/mac/shader-recipes', destination,
                        str(scale), tools], check=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(generate, range(1, 7)))


def main():
    global BUILD, APP
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, help='Destination containing MeleeUnlocked.app (default: build directory)')
    args = parser.parse_args()
    APP = (args.output_dir.resolve() if args.output_dir else BUILD)/'MeleeUnlocked.app'
    if not (BUILD/'MeleeUnlocked').is_file():
        raise SystemExit('Build the full host first: bash port/mac/build-full.sh')
    version = (ROOT/'VERSION').read_text().strip()
    manifest = BUILD/'build-manifest.json'
    if not manifest.is_file():
        raise SystemExit('Missing full-host build manifest; rebuild before packaging 0.9.56')
    built = json.loads(manifest.read_text())
    if (built.get('build_script') != 'port/mac/build_full.py' or
        built.get('host_entry') != 'port/app/main.cpp'):
        raise SystemExit('Packaging requires build_full.py with port/app/main.cpp; dev spike hosts cannot ship')
    if built.get('version') != version or built.get('host_sha256') != hashlib.sha256((BUILD/'MeleeUnlocked').read_bytes()).hexdigest():
        raise SystemExit('Full host does not match the current version/build manifest; rebuild first')
    if bool(built.get('squad_strike', False)) != squad_enabled():
        raise SystemExit('Squad Strike settings differ between the selected build and its manifest')
    for needed in [GAME_LIBRARY, GAME_LIBRARY.with_suffix('.snapexcl'), GAME_ASSETS/'Sys/GC/dsp_coef.bin']:
        if not needed.is_file():
            raise SystemExit(f'Missing native build input: {needed}')
    if APP.exists():
        shutil.rmtree(APP)
    contents = APP/'Contents'
    res = contents/'Resources'
    copy(BUILD/'MeleeUnlocked', contents/'MacOS/MeleeUnlocked')
    copy(ROOT.parent/'deps/sdl2/lib/libSDL2-2.0.0.dylib', contents/'Frameworks/libSDL2-2.0.0.dylib')
    # An sdl2-compat SDL2 (what Homebrew's sdl2 is now) loads SDL3 at run time from @loader_path/libSDL3.dylib.
    if (ROOT.parent/'deps/sdl2/lib/libSDL3.dylib').is_file():
        copy(ROOT.parent/'deps/sdl2/lib/libSDL3.dylib', contents/'Frameworks/libSDL3.dylib')
    copy(BUILD/'libusb-1.0.dylib', contents/'Frameworks/libusb-1.0.dylib')
    copy(ROOT/'port/third_party/libusb/COPYING', res/'licenses/libusb.txt')
    copy(GAME_LIBRARY,res/'melee_game.so')
    copy(GAME_LIBRARY.with_suffix('.snapexcl'),res/'melee_game.snapexcl')
    # Training Mode CE's data: the game build writes TM/ beside the library, and the host publishes TM/ beside
    # the loaded library (publish_training_files); without it Event Match is the retail one.
    if (GAME_ASSETS/'TM').is_dir():
        copy(GAME_ASSETS/'TM', res/'TM')
    for tool in ['glslangValidator','spirv-cross']:
        # copy2 follows the glslang symlink (equivalent to cp -L).
        copy(ROOT.parent/'deps/tools/bin'/tool,res/'bin'/tool)
    build_shader_seed(res/'shader-seed', res/'bin')
    # Linux release's runtime asset list, mapped to Resources. All mutable state lives in --user-dir.
    shutil.copytree(GAME_ASSETS/'Sys',res/'Sys',dirs_exist_ok=True,symlinks=False)
    for src, dst in [('port/slippi_sys','Sys'), ('port/slippi_sys_playback','SysPlayback'),
                     ('port/slippi_sys_general','slippi_sys_general')]:
        shutil.copytree(ROOT/src, res/dst, dirs_exist_ok=True, symlinks=False)
    copy(GAME_ASSETS/'Sys/GC/dsp_coef.bin',res/'Sys/GC/dsp_coef.bin')
    copy(GAME_ASSETS/'Sys/GC/dsp_coef.bin',res/'SysPlayback/GC/dsp_coef.bin')
    # Two of Slippi's Sys files are mostly Nintendo's own disc assets (compliance review, 10-25): they never ship. The
    # host rebuilds them from the player's disc (host/sysgen.cpp reads Resources/deltas, next to Sys).
    deltas = json.loads((ROOT/'port/sys_deltas/manifest.json').read_text())
    disc_derived = {f['target'] for f in deltas['files']}
    for folder in ['Sys','SysPlayback','slippi_sys_general']:
        for name in disc_derived:
            (res/folder/'GameFiles/GALE01'/name).unlink(missing_ok=True)
    (res/'deltas').mkdir()
    copy(ROOT/'port/sys_deltas/manifest.json',res/'deltas/manifest.json')
    for f in deltas['files']:
        copy(ROOT/'port/sys_deltas'/f['delta'],res/'deltas'/f['delta'])
    packaging = runpy.run_path(str(ROOT/'tools/package_linux_release.py'),run_name='mac_pack')
    (res/'Shaders').mkdir()
    (res/'Shaders/crt-shaders.pak').write_bytes(packaging['shader_pak']())
    for source in ['gd_melee','mockups','menu_previews','vendor']:
        copy(ROOT/'port/runtime/gx/ui_sources'/source/'assets',res/'ui_sources'/source)
    copy(ROOT/'docs/training-mods.md',res/'TrainingMods.md')
    # Keep the same notices as the Linux packaging script, without executing Linux packaging.
    import ast
    tree = ast.parse((ROOT/'tools/package_linux_release.py').read_text())
    for node in ast.walk(tree):
        if isinstance(node,ast.For) and isinstance(node.target,ast.Tuple) and [n.id for n in node.target.elts if isinstance(n,ast.Name)] == ['src','dst']:
            def rooted_path(expression):
                if isinstance(expression, ast.Name) and expression.id == 'ROOT':
                    return ROOT
                if isinstance(expression, ast.BinOp) and isinstance(expression.op, ast.Div):
                    return rooted_path(expression.left) / ast.literal_eval(expression.right)
                raise SystemExit('Unsupported source in Linux release license list')
            for pair in node.iter.elts:
                copy(rooted_path(pair.elts[0]), res/'licenses'/ast.literal_eval(pair.elts[1]))
    (res/'EXPERIMENTAL.txt').write_text('Experimental macOS build. See port/mac/README.md.\n')
    (res/'README.txt').write_text('Nucleus Unlocked macOS experimental build; unsigned.\n', encoding='utf-8')
    (res/'build-manifest.json').write_text(json.dumps(built, indent=2)+'\n')
    version = (ROOT/'VERSION').read_text().strip()
    info = dict(CFBundleIdentifier='io.github.0xburn.melee-unlocked', CFBundleName='Nucleus',
                CFBundleDisplayName='Nucleus Unlocked - Experimental', CFBundleExecutable='MeleeUnlocked',
                CFBundlePackageType='APPL',CFBundleShortVersionString=version, CFBundleVersion=version,
                LSMinimumSystemVersion='15.0',LSArchitecturePriority=['x86_64'],
                NSHighResolutionCapable=True,NSPrincipalClass='NSApplication')
    with (contents/'Info.plist').open('wb') as f:
        plistlib.dump(info,f)
    assert not any(p.is_symlink() for p in APP.rglob('*')), 'Bundle contains symlinks'
    shipped = sorted(str(p.relative_to(APP)) for p in APP.rglob('*') if p.name in disc_derived)
    assert not shipped, f'Bundle carries disc-derived Sys files: {shipped}'
    subprocess.run(['/usr/bin/otool','-L',str(contents/'MacOS/MeleeUnlocked')],check=True)
    print('Unsigned bundle:',APP)
if __name__ == '__main__':
    main()
