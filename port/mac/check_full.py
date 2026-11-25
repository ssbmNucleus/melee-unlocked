"""Isolated, muted, bounded M3 checks. Never uses the caller's account environment."""
from pathlib import Path
import argparse, csv, hashlib, json, os, plistlib, re, resource, socket, subprocess, sys, time
sys.dont_write_bytecode = True
from build_full import ROOT, BUILD, memory_gate, GAME_LIBRARY
from compare_digest import compare, load
REFERENCE = GAME_LIBRARY.parent / "digest-nosp.txt"
# MELEE_CHECK_DISC points a check at another local test image (e.g. a scrubbed copy), never an uploaded one.
DISC = os.environ.get('MELEE_CHECK_DISC', '/Volumes/YGWorktrees/build-out/test-discs/GALE01r2.iso')
EXE = BUILD/'MeleeUnlocked.app/Contents/MacOS/MeleeUnlocked'

def isolation(name):
    work = BUILD/'checks'/name
    if work.exists():
        previous = work.with_name(work.name + '-previous-' + str(time.time_ns()))
        work.rename(previous)  # keep earlier evidence; every check starts with an empty profile
    work.mkdir(parents=True, exist_ok=True)
    environment = {'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','LANG':'C','LC_ALL':'C',
        'HOME':str(work/'home'),'MELEE_NO_GC_ADAPTER':'1','SDL_AUDIODRIVER':'dummy','SDL_MAC_BACKGROUND_APP':'1','ROSETTA_ADVERTISE_AVX':'1'}
    for key, sub in [('XDG_DATA_HOME','data'),('XDG_CONFIG_HOME','config'),('XDG_CACHE_HOME','cache'),
                     ('XDG_STATE_HOME','state'),('XDG_RUNTIME_DIR','run'),('APPDATA','appdata'),
                     ('LOCALAPPDATA','localappdata'),('TMPDIR','tmp')]:
        environment[key] = str(work/sub)
    for key in ['HOME','XDG_DATA_HOME','XDG_CONFIG_HOME','XDG_CACHE_HOME','XDG_STATE_HOME','XDG_RUNTIME_DIR',
                'APPDATA','LOCALAPPDATA','TMPDIR']:
        Path(environment[key]).mkdir(exist_ok=True)
    args = [str(EXE),'--iso',DISC,'--headless','--volume','0',
        '--settings-path',str(work/'settings.ini'),'--sys-dir',str(EXE.parent.parent/'Resources/Sys'),
        '--user-dir',str(work/'user'),'--card-dir',str(work/'cards'),'--replay-dir',str(work/'replays'),
        '--log-file',str(work/'host.log'),'--shader-cache',str(work/'shaders')]
    return work, environment, args

def checked_run(command, work, environment, output='console.log', timeout=90):
    if command[0] == str(EXE):
        manifest = BUILD/'build-manifest.json'
        if not manifest.is_file():
            raise SystemExit('Rebuild the 0.9.56 host before running games (missing build manifest)')
        built = json.loads(manifest.read_text())
        with (EXE.parent.parent/'Info.plist').open('rb') as source:
            info = plistlib.load(source)
        if (built['version'] != (ROOT/'VERSION').read_text().strip() or
            info['CFBundleShortVersionString'] != built['version'] or
            hashlib.sha256(EXE.read_bytes()).hexdigest() != built['host_sha256'] or
            hashlib.sha256((EXE.parent.parent/'Resources/melee_game.so').read_bytes()).digest() !=
                hashlib.sha256(GAME_LIBRARY.read_bytes()).digest()):
            raise SystemExit('The packaged host/library are stale; rebuild and package before running games')
    memory_gate()
    with (work/output).open('w') as f:
        started = time.monotonic()
        p = subprocess.Popen(command, cwd=work, env=environment, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
        try:
            code = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
            print(f'TIMEOUT: killed owned PID {p.pid}: {output}', flush=True)
            code = 124
        if p.stdin: p.stdin.close()
    elapsed = time.monotonic()-started
    for path in work.rglob('*.log'):
        if path.exists() and 'slippi: logged in as' in path.read_text(errors='replace'):
            raise SystemExit(f'FAIL: Slippi login detected in {path}')
    print(f'{output}: exit {code}, wall {elapsed:.3f}s',flush=True)
    return code

def parity(graphics=False, seed=False, sync=False, windowed=False, fullscreen=False, video_test=None, scale='1'):
    mode = 'sync' if sync else 'async'
    name = ('graphics' if graphics else 'parity') + '-' + mode + ('-seed' if seed else '-cold')
    if video_test: name += '-' + video_test
    work, env, args = isolation(name)
    env['MELEE_SYNC_SHADERS'] = '1' if sync else '0'
    seed_path = EXE.parent.parent/'Resources/shader-seed'
    if seed and not (seed_path/('gx-gl410-v2-s'+scale)/'recipes.bin').is_file():
        raise SystemExit('Missing bundle seed: ' + str(seed_path))
    # Empty is an explicit opt-out of full_launch.h's bundle default.
    env['MELEE_SHADER_SEED'] = str(seed_path) if seed else ''
    env['MELEE_SIM_TIMES'] = str(work/'frame-times.csv')
    if graphics:
        if windowed:
            args.remove('--headless')  # Lead only: never run this option inside the sandbox.
            env.pop('SDL_MAC_BACKGROUND_APP', None)
        else:
            args[args.index('--headless')] = '--hidden'
        env['MELEE_TEST_SURFACE'] = '1'
        env['MELEE_TEST_SHOT'] = str(work) + ':30,180,330,420,600,1200,1800'
        if fullscreen: args += ['--fullscreen']
        controls = {'resize':('MELEE_TEST_RESIZE','1600x1000@120'),
                    'sdl':('MELEE_TEST_FULLSCREEN','120'), 'appkit':('MELEE_TEST_TOGGLE','120'),
                    'escape':('MELEE_TEST_ESCAPE','180'), 'menu':('MELEE_TEST_ESCAPE','180')}
        if video_test:
            key, value = controls[video_test]; env[key] = value
        if video_test in ('sdl','appkit'): env['MELEE_TEST_ESCAPE'] = '300'
        args += ['--scale',scale]
    args += ['--fast']
    args += ['--frames','9000','--vanilla-game','--time-base','0','--rng-seed','0x12345678',
             '--script',str(ROOT/'port/scripts/parity_vs.txt'),'--state-digest',str(work/'digest.csv'),
             '--threaded-renderer','--vanilla-only']
    import shlex
    (work/'command.txt').write_text(shlex.join(args)+'\n')
    (work/'environment.json').write_text(json.dumps(env,indent=2)+'\n') # Only the explicitly constructed test environment.
    code = checked_run(args,work,env,timeout=120)
    if code:
        return code
    result = compare(REFERENCE, work/'digest.csv')
    cache = re.search(r'opengl: shader cache: (\d+) hits, (\d+) translation misses', (work/'console.log').read_text())
    if cache: result['shader_cache'] = {'hits':int(cache[1]),'translation_misses':int(cache[2])}
    if graphics and seed and (not cache or not int(cache[1])): result['passed'] = False
    (work/'compare.txt').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)
    timings = work/'frame-times.csv'
    if timings.exists():
        with timings.open(newline='') as f:
            times = [float(r['sim_ms']) for r in csv.DictReader(f)]
        summary = {'rendering':graphics, 'mode':mode, 'seed':seed, 'frames':len(times),
                   'hitches_over_33ms':sum(t > 33 for t in times), 'worst_ms':max(times,default=0)}
        (work/'timing.json').write_text(json.dumps(summary,indent=2)+'\n')
        print(json.dumps(summary),flush=True)
    return int(not result['passed'])


def memory_card():
    work, env, args = isolation('memory-card')
    assert not (work/'cards').exists()
    args += ['--frames','1800','--fast','--vanilla-game','--vanilla-only',
             '--time-base','0','--rng-seed','0x12345678','--state-digest',str(work/'digest.csv')]
    code = checked_run(args,work,env,timeout=120)
    if code: return code
    log = (work/'host.log').read_text()
    # The boot scene (major 40) must write the save and move on. The host's "auto-confirm decision"
    # log lines this check used to require went with the YouGame code (fe885720, 2026-11-22).
    mounts = re.findall(r'card: source slot A mounted from .* \((\d+) files',log)
    saves = [path for path in (work/'cards').rglob('*') if path.is_file() and 'GALE01' in path.name]
    rows, _ = load(work/'digest.csv')
    scenes = []
    for row in rows:
        scene = [row['scene_major'],row['scene']]
        if not scenes or scenes[-1][1:] != scene:
            scenes.append([row['frame'],*scene])
    left_boot = any(s[1] == 40 for s in scenes) and scenes[-1][1] != 40
    passed = bool(saves) and left_boot and bool(mounts) and mounts[0] == '0' and mounts[-1] != '0'
    result = dict(passed=passed, mounted_files=mounts, saves=[str(p.relative_to(work)) for p in saves],
                  scenes=scenes)
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)
    return int(not passed)


def translation_check():
    work, env, _ = isolation('translation')
    exe = work/'translation-check'
    memory_gate()
    with (work/'build.log').open('w') as log:
        built = subprocess.run(['/usr/bin/clang++','-arch','x86_64','-mmacosx-version-min=15.0','-std=c++17',
            '-O2','-pthread',str(ROOT/'port/mac/translation_check.cpp'),'-o',str(exe)],stdout=log,stderr=subprocess.STDOUT)
    if built.returncode: return built.returncode
    return checked_run([str(exe),str(work),str(EXE.parent.parent/'Resources/bin')],work,env,timeout=120)


def seed_build():
    work, env, _ = isolation('seed-build')
    exe = work/'seed-build'
    memory_gate()
    with (work/'build.log').open('w') as log:
        built = subprocess.run(['/usr/bin/clang++','-arch','x86_64','-mmacosx-version-min=15.0','-std=c++17',
            '-O2','-pthread','-I'+str(ROOT/'port/runtime/gx'),str(ROOT/'port/mac/seed_build.cpp'),
            str(BUILD/'objects/port_runtime_gx_gx_shader.cpp.o'),str(BUILD/'objects/port_runtime_gx_gx_texture.cpp.o'),
            '-o',str(exe)],stdout=log,stderr=subprocess.STDOUT)
    if built.returncode: return built.returncode
    return checked_run([str(exe),str(ROOT/'port/mac/shader-recipes'),str(work/'seed'),'2',
                        str(EXE.parent.parent/'Resources/bin')],work,env,timeout=120)


def units():
    work, env, _ = isolation('unit')
    unit = BUILD/'unit'
    memory_gate()
    configured = subprocess.run(['cmake','-S',str(ROOT),'-B',str(unit),'-DCMAKE_OSX_ARCHITECTURES=x86_64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0','-DCMAKE_CXX_COMPILER=/usr/bin/clang++',
        '-DCMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/python3;'+str(ROOT/'port/mac/memory_gate.py')],
        stdout=(work/'configure.log').open('w'),stderr=subprocess.STDOUT)
    if configured.returncode:
        return configured.returncode
    memory_gate()
    with (work/'build.log').open('w') as log:
        built = subprocess.run(['cmake','--build',str(unit),'--parallel','2'],stdout=log,stderr=subprocess.STDOUT)
    if built.returncode:
        return built.returncode
    specs = json.loads(subprocess.check_output(['ctest','--test-dir',str(unit),'--show-only=json-v1'],text=True))['tests']
    results = []
    for spec in specs:
        code = checked_run(spec['command'],unit,env,output=spec['name']+'.log')
        properties = {p['name']: p['value'] for p in spec.get('properties', [])}
        skipped = code == properties.get('SKIP_RETURN_CODE')
        results.append({'test':spec['name'],'exit':code,'skipped':skipped})
    (work/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'Unit tests: {sum(r["exit"]==0 for r in results)}/{len(results)} passed, '
          f'{sum(r["skipped"] for r in results)} skipped',flush=True)
    return int(any(r['exit'] and not r['skipped'] for r in results))

def policies():
    """Offline policy regressions on both Mac architectures."""
    work, env, _ = isolation('policies')
    results = []
    for arch in ['arm64', 'x86_64']:
        label = 'lcancel-'+arch
        exe = work/label
        code = checked_run(['/usr/bin/clang++', '-arch', arch, '-mmacosx-version-min=15.0', '-O2',
                            '-std=c++17', '-I'+str(ROOT/'port/runtime/host'), str(ROOT/'port/tests/lcancel_policy_test.cpp'), '-o', str(exe)],
                           work, env, output=label+'-build.log', timeout=120)
        if code: return code
        code = checked_run([str(exe)], work, env, output=label+'.log', timeout=30)
        results.append(dict(test=label, exit=code))
    (work/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    print(f'Policy/source checks: {sum(r["exit"]==0 for r in results)}/{len(results)} passed',flush=True)
    return int(any(r['exit'] for r in results))

def netplay():
    work, env, _ = isolation('netplay')
    # Stop this step immediately if the sandbox refuses loopback sockets.
    try:
        probe = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        probe.bind(('127.0.0.1',0)); probe.close()
    except OSError as e:
        (work/'refused.txt').write_text(f'Loopback UDP bind refused: {e}\n')
        print(f'Loopback UDP bind refused: {e}',flush=True)
        return 77
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        codes = list(pool.map(peer,range(2)))
    return int(any(codes))

def peer(i):
    local, remote = (43171,43172) if i==0 else (43172,43171)
    work, env, args = isolation(f'netplay-{i}')
    args += ['--frames','1200','--online-test','direct','--local-peer',f'{i}:{local}:127.0.0.1:{remote}',
             '--test-stage','2','--rng-seed','0x12345678','--time-base','0','--state-digest',str(work/'digest.csv')]
    import shlex
    (work/'command.txt').write_text(shlex.join(args)+'\n')
    code = checked_run(args,work,env)
    if code: return code
    with (work/'digest.csv').open(newline='') as f:
        matches = sum(int(r['match_frame'],16) > 0 for r in csv.DictReader(f))
    print(f'Peer {i}: {matches} match frames',flush=True)
    return int(matches == 0)


def compatibility():
    work, env, _ = isolation('compat')
    exe = work/'compat-test'
    memory_gate()
    with (work/'build.log').open('w') as log:
        built = subprocess.run(['/usr/bin/clang++','-arch','x86_64','-mmacosx-version-min=15.0','-std=c++17',
            '-I'+str(ROOT/'port/linux/compat'),str(ROOT/'port/mac/compat_test.cpp'),str(BUILD/'libfull_host.a'), str(BUILD/'libusb-1.0.dylib'), '-Wl,-rpath,'+str(BUILD),
            '-L'+str(ROOT.parent/'deps/sdl2/lib'),'-lSDL2-2.0.0','-lcurl','-liconv','-framework','OpenGL','-framework','AppKit','-framework','Security',
            '-Wl,-rpath,'+str(ROOT.parent/'deps/sdl2/lib'),'-o',str(exe)],stdout=log,stderr=subprocess.STDOUT)
    if built.returncode: return built.returncode
    return checked_run([str(exe)],work,env)

def main():
    global BUILD, EXE
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('check',choices=['parity','graphics','shaders','seed-build','unit','policies','netplay','compat','memory-card','peer-0','peer-1'])
    parser.add_argument('--seed',action='store_true')
    parser.add_argument('--sync-shaders',action='store_true')
    parser.add_argument('--windowed',action='store_true',help='Lead only, outside the sandbox: show the graphics window')
    parser.add_argument('--fullscreen',action='store_true')
    parser.add_argument('--scale',default='1')
    parser.add_argument('--video-test',choices=['resize','sdl','appkit','escape','menu'])
    args = parser.parse_args()
    EXE = BUILD/'MeleeUnlocked.app/Contents/MacOS/MeleeUnlocked'
    run_parity = lambda graphics: parity(graphics,args.seed,args.sync_shaders,args.windowed,args.fullscreen,args.video_test,args.scale)
    return {'parity':lambda:run_parity(False),'graphics':lambda:run_parity(True),'shaders':translation_check,'seed-build':seed_build,'unit':units,'policies':policies,'netplay':netplay,'compat':compatibility,'memory-card':memory_card,'peer-0':lambda:peer(0),'peer-1':lambda:peer(1)}[args.check]()
if __name__ == '__main__':
    sys.exit(main())
