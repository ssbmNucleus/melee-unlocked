#!/usr/bin/env python3
"""Play a Ranked set between the native fork and Slippi Dolphin 3.7.0 on loopback.

macOS: both games run under sandbox-exec, which permits only loopback traffic, and libmelee
reads stock Dolphin's menus through its spectator port.

Linux: the whole run (this script, the stand-in queue, Xvfb and both games) re-executes itself
under `unshare -rn`, a new user and network namespace whose only interface is loopback. Before
any game starts it checks that the namespace has no route and resolves no names, and writes that
proof to isolation.txt. The resolvers named in /etc/resolv.conf are then answered inside the
namespace by a recorder that refuses every name, so lookups.log lists what each game tried to
reach. Stock Dolphin is the official AppImage, extracted and unchanged; it gets a fake HOME, a
fabricated identity in its own User folder and a named-pipe controller. It renders on a private
Xvfb (software rendering) or, with --display, on a desktop's X display and GPU. The harness reads the
emulated console's memory through /proc (read only) to steer Dolphin through its menus, the
Ranked setup and the games, so libmelee is not needed there.
"""
import argparse
import asyncio
from collections import Counter
from contextlib import ExitStack
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

try:
    import melee  # libmelee: the macOS path reads stock Dolphin's menus through it
except ImportError:
    melee = None
try:
    import ubjson
except ImportError:
    ubjson = None
from websockets.asyncio.server import serve

from check_local_ranked_set import controller, two_ports


def local_user(seat):
    return {"uid": f"fixture{seat}", "playKey": "local-test-only", "displayName": ("Native", "Dolphin")[seat],
            "connectCode": f"TEST#{20 + seat}", "latestVersion": "3.7.0"}


def prepare_stock(app, work, spectator_port, peer_port):
    user = work / "User"
    account = work / "home/Library/Application Support/com.project-slippi.dolphin/Slippi"
    account.mkdir(parents=True)
    (account / "user.json").write_text(json.dumps(local_user(1)))
    config = write_stock_config(user, work, spectator_port, peer_port)
    pad = (app / "Contents/Resources/Config/Profiles/GCPad/slippibot.ini").read_text()
    (config / "GCPadNew.ini").write_text(pad.replace("[Profile]", "[GCPad1]"))
    # This adds libmelee's documented menu telemetry for the test controller.
    # The stock app, disc, online game codes and gameplay rules stay intact.
    name = "$Optional: Extract Menu Info"
    codes = (Path(melee.__file__).parent / "GALE01r2.ini").read_text()
    definition = next(block for block in codes.split("\n$") if block.startswith(name[1:] + " ["))
    (user / "GameSettings").mkdir()
    (user / "GameSettings/GALE01r2.ini").write_text(f"[Gecko_Enabled]\n{name}\n[Gecko]\n${definition}\n")
    pipes = user / "Pipes"
    pipes.mkdir()
    path = pipes / "slippibot1"
    os.mkfifo(path)
    return path


def write_stock_config(user, work, spectator_port, peer_port):
    """Dolphin.ini and Logger.ini of the stock client's User folder; the same on both systems."""
    config = user / "Config"
    config.mkdir(parents=True)
    (config / "Dolphin.ini").write_text(f"""[Core]
EnableCheats = True
DSPHLE = True
SIDevice0 = 6
SIDevice1 = 0
SIDevice2 = 0
SIDevice3 = 0
SlippiEnableSpectator = True
SlippiSpectatorLocalPort = {spectator_port}
SlippiForceNetplayPort = True
SlippiNetplayPort = {peer_port}
SlippiForceLanIp = True
SlippiLanIp = 127.0.0.1
SlippiSaveReplays = True
SlippiReplayMonthFolders = False
SlippiReplayDir = {work / 'games'}
SlippiJukeboxEnabled = False
BlockingPipes = False
[Interface]
PanicHandlers = False
ConfirmStop = False
PauseOnFocusLost = False
[Display]
Fullscreen = False
RenderToMain = True
[Input]
BackgroundInput = True
[DSP]
Backend = No audio output
Volume = 0
[Analytics]
Enabled = False
PermissionAsked = True
""")
    (config / "Logger.ini").write_text("[Options]\nWriteToFile=True\nVerbosity=4\n[Logs]\n"
                                       "SLIPPI=True\nSLIPPI_ONLINE=True\nSLIPPI_RUST_ONLINE=True\n")
    return config


def start_game(stack, command, work, url, extra=None):
    # Some Apple APIs still use the account home from the OS even with HOME set.
    # Deny both real credential directories and permit only loopback outbound traffic.
    protected = [Path.home() / "Library/Application Support" / name
                 for name in ("Slippi Launcher", "com.project-slippi.dolphin")]
    filters = " ".join(f"(subpath {json.dumps(str(path))})" for path in protected)
    sandbox = ('(version 1)(allow default)(deny network-outbound)'
               '(allow network-outbound (remote ip "localhost:*"))'
               f'(deny file-read* file-write* {filters})')
    (work / "home").mkdir(exist_ok=True)
    log = stack.enter_context((work / "console.log").open("wb"))
    env = {"PATH": os.environ["PATH"], "HOME": str(work / "home"), "NO_PROXY": "*", "SLIPPI_MM_URL": url,
           **(extra or {})}
    proc = subprocess.Popen(["/usr/bin/sandbox-exec", "-p", sandbox, *command], cwd=work,
                            stdout=log, stderr=subprocess.STDOUT, env=env)

    def stop():
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    stack.callback(stop)
    return proc


class Queue:
    def __init__(self):
        self.joins = {}
        self.ready = asyncio.Event()

    async def handle(self, ws):
        join = json.loads(await ws.recv())
        seat = next(i for i in range(2) if join["user"]["uid"] == local_user(i)["uid"])
        if (ws.request.path, join["type"], join["appVersion"], join["search"]["mode"]) != ("/v1/queue", "join", "3.7.0", 0):
            raise ValueError("Expected a 3.7.0 Ranked join")
        if seat in self.joins:
            raise ValueError("Unexpected second join by the same client")
        self.joins[seat] = join
        print(f"Ranked join from {local_user(seat)['displayName']}, peer port {join['port']}", flush=True)
        await ws.send(json.dumps({"type": "joined", "token": "local", "ticketId": str(seat)}))
        if len(self.joins) == 2:
            self.ready.set()
        until = time.monotonic() + 90
        while not self.ready.is_set():
            if time.monotonic() >= until:
                raise TimeoutError("The second local client did not join")
            await ws.send(json.dumps({"type": "status", "queuePosition": 1}))
            try:
                await asyncio.wait_for(self.ready.wait(), 2)
            except asyncio.TimeoutError:
                continue
        players = []
        for i in range(2):
            user = local_user(i)
            address = f"127.0.0.1:{int(self.joins[i]['port'])}"
            players.append({key: user[key] for key in ("uid", "displayName", "connectCode")} |
                           {"port": i + 1, "isLocalPlayer": i == seat, "ipAddress": address, "ipAddressLan": address})
        await ws.send(json.dumps({"type": "match", "matchId": "mode.ranked-cross-test", "isHost": seat == 0,
                                  "players": players, "stages": [2, 3, 8, 28, 31, 32]}))
        await ws.wait_closed()


def drive_stock(pipe, state, tick):
    if tick % 9 == 4:
        os.write(pipe, b"RELEASE A\nRELEASE START\nRELEASE D_RIGHT\n")
    if tick % 9 or state is None:
        return
    commands = ["RELEASE A", "RELEASE START", "RELEASE D_RIGHT", "SET MAIN 0.5 0.5"]
    scene = state.menu_state
    if scene == melee.Menu.MAIN_MENU:
        row = state.submenu == melee.SubMenu.ONLINE_PLAY_SUBMENU and state.menu_selection != 0
        commands.append("SET MAIN 0.5 0" if row else "PRESS A")
    elif scene == melee.Menu.PRESS_START:
        commands.append("PRESS START")
    elif scene == melee.Menu.SLIPPI_ONLINE_CSS and 1 in state.players:
        player = state.players[1]
        # A can pick the character token back up. Leave several Start pulses
        # between A presses so the two transitions can be observed at 60 Hz.
        locked_or_hovering = player.coin_down or player.character != melee.Character.UNKNOWN_CHARACTER
        if locked_or_hovering and tick // 9 % 11:
            commands.append("PRESS START")
        elif player.cursor.y < 15:
            commands.append("SET MAIN 0.5 1")
        elif player.cursor.x < -10:
            commands.append("SET MAIN 1 0.5")
        else:
            commands.append("PRESS A")
    elif scene == melee.Menu.IN_GAME:
        commands.append("SET MAIN 1 0.5")  # Walk the stock-side fighter off each respawn.
    else:
        commands.append("PRESS A" if tick % 27 else "PRESS D_RIGHT")
    os.write(pipe, ("\n".join(commands) + "\n").encode())


def slp_raw(path):
    """The event stream of a .slp recording (its UBJSON "raw" byte array)."""
    data = path.read_bytes()
    if ubjson is not None:
        return ubjson.loadb(data)["raw"]
    head = b"{U\x03raw[$U#l"
    if not data.startswith(head):
        raise ValueError(f"Unexpected recording header in {path}")
    start = len(head) + 4
    length = int.from_bytes(data[len(head):start], "big")
    if length == 0:  # a recording that was never finalized: up to the metadata, if any
        end = data.find(b"U\x08metadata", start)
        length = (end if end > 0 else len(data)) - start
    return data[start:start + length]


def fighter_records(path):
    raw = slp_raw(path)
    if raw[0] != 0x35:
        raise ValueError(f"Missing event size table in {path}")
    end = raw[1] + 1
    sizes = {raw[j]: int.from_bytes(raw[j + 1:j + 3], "big") + 1 for j in range(2, end, 3)}
    offset = end
    records, ending = {}, None
    while offset < len(raw):
        event = raw[offset]
        body = raw[offset:offset + sizes[event]]
        if event == 0x38:
            key = (int.from_bytes(body[1:5], "big", signed=True), body[5], body[6])
            records[key] = body  # Keep the final simulation of this frame after any rollback.
        elif event == 0x39:
            ending = body
        offset += len(body)
    return records, ending


def compare_game(native_path, stock_path):
    native, end_native = fighter_records(native_path)
    stock, end_stock = fighter_records(stock_path)
    if not native or native.keys() != stock.keys() or end_native != end_stock or end_native is None:
        raise ValueError(f"Frame coverage or game end differs: {native_path.name}, {stock_path.name}")
    fields = Counter()
    identical = 0
    for key, a in native.items():
        b = stock[key]
        if len(a) != len(b):
            raise ValueError(f"Post-frame length differs at {key}")
        identical += a == b
        for pos, (left, right) in enumerate(zip(a, b)):
            if left != right:
                fields[(pos, left ^ right)] += 1
    print(f"{native_path.name}: {identical}/{len(native)} byte-identical fighter records; "
          f"differing (byte, mask) counts {dict(fields)}", flush=True)
    return fields


async def run(args, work, stock_app):
    native = work / "fork"
    (native / "user").mkdir(parents=True)
    (native / "user/user.json").write_text(json.dumps(local_user(0)))
    (native / "pad.txt").write_text(controller(0))
    fork_env = {"MELEE_NO_GC_ADAPTER": "1", "SDL_AUDIODRIVER": "dummy",
                "MELEE_TEST_SCRIPT_PAD_FRAMES": "1", "ROSETTA_ADVERTISE_AVX": "1"}
    stock = work / "stock"
    peer_port, spectator_port = two_ports()
    fifo = prepare_stock(stock_app, stock, spectator_port, peer_port)
    queue = Queue()
    async with serve(queue.handle, "127.0.0.1", 0) as server:
        url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/v1/queue"
        with ExitStack() as stack:
            pipe = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            stack.callback(os.close, pipe)
            old = start_game(stack, [stock_app / "Contents/MacOS/Slippi Dolphin", "-u", stock / "User",
                                     "-b", "-c", "False", "-e", args.iso, "-v", "OGL", "--slippi-spectator-port",
                                     str(spectator_port), "-a", "HLE"], stock, url)
            new = start_game(stack, [args.fork / "Contents/MacOS/MeleeUnlocked", "--iso", args.iso, "--headless",
                                     "--allow-matchmaking", "--volume", "0", "--slippi-menus", "on",
                                     "--user-dir", native / "user", "--sys-dir", args.fork / "Contents/Resources/Sys",
                                     "--card-dir", native / "cards", "--replay-dir", native / "games",
                                     "--settings-path", native / "settings.ini", "--script", native / "pad.txt",
                                     "--frames", "24000", "--log-file", native / "host.log"], native, url,
                             fork_env)
            await asyncio.sleep(7)
            spectator = melee.Console(is_remote=True, slippi_port=spectator_port, polling_mode=True, setup_gecko_codes=False)
            if not await asyncio.to_thread(spectator.connect):
                raise RuntimeError("Could not connect to the stock spectator port")
            stack.callback(spectator._slippstream.shutdown)
            last_scene = None
            state, tick = None, 0
            deadline = time.monotonic() + 420
            while time.monotonic() < deadline:
                if old.poll() is not None or new.poll() is not None:
                    raise RuntimeError("A game exited early")
                fresh = spectator.step()
                if fresh is not None:
                    state = fresh
                    if state.menu_state != last_scene:
                        print(f"Stock scene: {state.menu_state.name}", flush=True)
                        last_scene = state.menu_state
                tick += 1
                drive_stock(pipe, state, tick)
                if tick % 45 == 0:
                    host = (native / "host.log").read_text(errors="replace")
                    if "ranked set: over" in host:
                        break
                    if "opponent step invalid" in host or "left before both seats finished" in host:
                        raise RuntimeError("Ranked setup ended before both players completed stage selection")
                    if "ranked set: abandoned" in host:
                        raise RuntimeError("Ranked set was abandoned")
                await asyncio.sleep(.015)
            else:
                raise RuntimeError("Timed out waiting for the Ranked set")
            await asyncio.sleep(2)  # Allow the stock recorder to finalize its file.
            stock_log = (stock / "User/Logs/dolphin.log").read_text(errors="replace")
            fork_reports = re.findall(r"game report: mode 0, (\d+) frames, game (\d+), tiebreak 0, winner (\d+), end 2, lras -1, stage (\d+)", host)
            stock_reports = re.findall(r"Frames: (\d+), GameIdx: (\d+), TiebreakIdx: 0, WinnerIdx: (\d+), StageId: (\d+), GameEndMethod: 2, LRASInitiator: -1", stock_log)
            if len(fork_reports) < 2 or fork_reports != stock_reports or len(queue.joins) != 2:
                raise ValueError(f"Conflicting game reports: fork={fork_reports}, stock={stock_reports}")
            print(f"Both clients completed the Ranked set: {fork_reports}", flush=True)
            native_games = sorted((native / "games").glob("*.slp"))
            stock_games = sorted((stock / "games").glob("*.slp"))
            if len(native_games) != len(stock_games) or len(native_games) != len(fork_reports):
                raise ValueError("A game recording is missing")
            for a, b in zip(native_games, stock_games):
                differences = compare_game(a, b)
                if any(field != (0x2A, 0x80) for field in differences):
                    raise ValueError("Final fighter state differs outside the recorded offscreen bit")


# ---------------------------------------------------------------- Linux

LINUX_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
OUTER_NETNS = "MU_STOCK_SET_OUTER_NETNS"
PROBE_HOSTS = ("users-rest-dot-slippi.uc.r.appspot.com", "internal.slippi.gg", "matchmaking.slippi.gg",
               "stun.l.google.com", "github.com")
# NTSC 1.02 addresses, from the decomp's symbols (port/runtime reads the same ones).
SCENE = 0x80479D30        # state_machine: +0 major scene, +3 minor scene
SCENE_FRAMES = 0x80479D58 # gm_80479D58.unk_0: engine frames since the scene started
MATCH_FRAMES = 0x8046B6C4 # VsSceneController state frame count: the match's frame
MENU_FLOW = 0x804A04F0    # mnmain MenuFlow: +0 menu kind, +2 hovered row (u16)
ONLINE_MODE = 0x804D6640  # the mode picked in Slippi's online menu: 0 Ranked, 1 Unranked, 2 Direct, 3 Teams
CSS_CURSORS = 0x804A0BC0  # mnCharSel hands: +5 state (1 holds the coin), +0xC x, +0x10 y
CSS_COINS = 0x804A0BD0    # mnCharSel coins: +5 state, +8 x, +0xC y
CSS_DOORS = 0x803F0DFC    # mnCharSel doors, 0x24 each: +0xE the icon under the coin (25: none)
PLAYERS = 0x80453080      # player blocks, 0xE90 each: +0 state (2: in a match), +0x8E stocks, +0xB0 fighter
ONLINE_MAJOR, CSS_MINOR, GAME_MINOR, SETUP_MINOR = 8, 0, 2, 5
# Ledge x of the legal stages by Slippi stage id: FoD, Pokemon Stadium, Yoshi's Story, Dream Land,
# Battlefield, Final Destination.
STAGE_EDGES = {2: 63.3, 3: 87.7, 8: 56.0, 28: 77.2, 31: 68.4, 32: 85.5}
# External character id of each character select icon, left to right, top to bottom.
ICON_CHARACTER = (0x16, 8, 7, 5, 0xC, 0x11, 1, 0, 0x19, 0x14, 2, 0xB, 0xE, 4, 0x10, 0x12, 6, 0x15,
                  0x18, 0xD, 0xF, 0xA, 3, 9, 0x17)
# The fork in a match: jab, shield, crouch, grab and up tilt in place, every 4 seconds.
FORK_MATCH = ["@match", "0", "20 A", "23", "60 R", "85", "110 sy=-127", "130", "160 Z", "163",
              "200 sy=60 A", "205", "@loop 240"]


def icon_center(icon):
    """Middle of a retail character select icon (mncharsel.c icons[] bounds)."""
    cols = (-30.0, -24.4, -17.4, -10.4, -3.4, 3.6, 10.6, 17.6, 24.4, 30.2)
    if icon < 18:
        row, col = divmod(icon, 9)
        return (cols[col] + cols[col + 1]) / 2, (16.5, 9.5)[row]
    bottom = [(-23.4, -17.4)] + [(cols[i], cols[i + 1]) for i in range(2, 7)] + [(17.6, 23.6)]
    left, right = bottom[icon - 18]
    return (left + right) / 2, 2.5


def picker_path(start, target):
    """D-pad presses that move Slippi's Ranked character picker (7 columns, 27 slots) from one
    character to another; CharPickerDialog's moves as port/../mu_ranked_widgets.c models them."""
    def move(c, d):
        row, col = divmod(c, 7)
        width = 6 if row == 3 else 7
        if d == "D_RIGHT":
            return row * 7 + (col + 1) % width
        if d == "D_LEFT":
            return row * 7 + (col + width - 1) % width
        row = (row + 1) % 4 if d == "D_DOWN" else (row + 3) % 4
        if row == 3 and col == 6:
            row = 0 if d == "D_DOWN" else 2
        return row * 7 + col
    paths, frontier = {start: []}, [start]
    while frontier and target not in paths:
        nxt = []
        for c in frontier:
            for d in ("D_RIGHT", "D_DOWN", "D_LEFT", "D_UP"):
                n = move(c, d)
                if n not in paths:
                    paths[n] = paths[c] + [d]
                    nxt.append(n)
        frontier = nxt
    return paths[target]


def enter_network_namespace():
    """Run this script again as `unshare -rn ...`: a new user and network namespace, loopback only."""
    unshare = shutil.which("unshare", path=LINUX_PATH)
    if not unshare:
        raise SystemExit("unshare is not available")
    env = dict(os.environ, **{OUTER_NETNS: os.readlink("/proc/self/ns/net")})
    os.execve(unshare, [unshare, "-rn", "--", sys.executable, os.path.abspath(sys.argv[0]), *sys.argv[1:]], env)


def isolate_network(work):
    """Bring loopback up and prove there is no other way out, before anything else starts."""
    lines = []

    def note(text):
        lines.append(text)
        print(f"isolation: {text}", flush=True)

    outer, inner = os.environ.get(OUTER_NETNS), os.readlink("/proc/self/ns/net")
    if not outer or outer == inner:
        raise RuntimeError("Not inside a new network namespace")
    note(f"network namespace {inner}, started from {outer}; uid map {' '.join(Path('/proc/self/uid_map').read_text().split())}")
    subprocess.run([shutil.which("ip", path=LINUX_PATH), "link", "set", "lo", "up"], check=True)
    # /proc/net follows this process's network namespace (/sys/class/net still shows the outer one).
    interfaces = sorted(line.split(":")[0].strip() for line in Path("/proc/net/dev").read_text().splitlines()[2:])
    note(f"interfaces: {', '.join(interfaces)}")
    if interfaces != ["lo"]:
        raise RuntimeError("The namespace has an interface other than lo")
    routes4 = Path("/proc/net/route").read_text().splitlines()[1:]
    routes6 = [line.split() for line in Path("/proc/net/ipv6_route").read_text().splitlines()]
    elsewhere = [r for r in routes6 if r[-1] != "lo"]
    note(f"IPv4 routes: {len(routes4)}; IPv6 routes: {len(routes6)}, all on lo: {not elsewhere}")
    if routes4 or elsewhere:
        raise RuntimeError("The namespace has a route off loopback")
    # Name services that answer through a file socket would be outside the namespace: show them.
    hosts = re.findall(r"^\s*hosts:.*$", Path("/etc/nsswitch.conf").read_text(), re.M)
    note(f"nsswitch {hosts[0].strip() if hosts else 'hosts: (none)'}; resolv.conf nameservers "
         f"{re.findall(r'^\s*nameserver\s+(\S+)', Path('/etc/resolv.conf').read_text(), re.M)}")
    for socket_path in ("/var/run/nscd/socket", "/run/systemd/resolve/io.systemd.Resolve"):
        note(f"{socket_path}: {'present' if Path(socket_path).exists() else 'absent'}")
    for host in PROBE_HOSTS:
        try:
            found = socket.getaddrinfo(host, 443)
        except socket.gaierror as error:
            note(f"lookup {host}: fails ({error})")
        else:
            raise RuntimeError(f"{host} resolved inside the namespace: {found}")
    for family, kind, address in ((socket.AF_INET, socket.SOCK_STREAM, ("1.1.1.1", 443)),
                                  (socket.AF_INET, socket.SOCK_STREAM, ("140.82.112.3", 443)),
                                  (socket.AF_INET, socket.SOCK_DGRAM, ("8.8.8.8", 53)),
                                  (socket.AF_INET6, socket.SOCK_STREAM, ("2606:4700:4700::1111", 443)),
                                  (socket.AF_INET6, socket.SOCK_DGRAM, ("2001:4860:4860::8888", 53))):
        with socket.socket(family, kind) as probe:
            probe.settimeout(5)
            try:
                if kind == socket.SOCK_STREAM:
                    probe.connect(address)
                else:
                    probe.sendto(b"\0", address)
            except OSError as error:
                if error.errno not in (errno.ENETUNREACH, errno.EHOSTUNREACH, errno.EADDRNOTAVAIL):
                    raise RuntimeError(f"Unexpected answer from {address}: {error}")
                note(f"{'tcp' if kind == socket.SOCK_STREAM else 'udp'} {address[0]} port {address[1]}: {error.strerror}")
            else:
                raise RuntimeError(f"{address} was reachable from inside the namespace")
    (work / "isolation.txt").write_text("\n".join(lines) + "\n")


class LookupRecorder:
    """Answers, inside the namespace, at the resolver addresses /etc/resolv.conf names: every
    question is written to lookups.log with the process that asked, and refused (NXDOMAIN).
    Nothing is forwarded."""

    def __init__(self, path):
        self.log = path.open("a", buffering=1)
        self.names = Counter()
        self.lock = threading.Lock()

    def start(self):
        ip = shutil.which("ip", path=LINUX_PATH)
        servers = re.findall(r"^\s*nameserver\s+([0-9.]+)\s*$", Path("/etc/resolv.conf").read_text(), re.M)
        for server in dict.fromkeys(servers + ["127.0.0.53"]):
            if not server.startswith("127."):
                subprocess.run([ip, "addr", "add", f"{server}/32", "dev", "lo"], check=True)
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind((server, 53))
            threading.Thread(target=self.serve, args=(sock,), daemon=True).start()
        return servers

    def serve(self, sock):
        while True:
            packet, peer = sock.recvfrom(4096)
            if len(packet) < 17:
                continue
            labels, pos = [], 12
            while pos < len(packet) and packet[pos] and len(labels) < 64:
                labels.append(packet[pos + 1:pos + 1 + packet[pos]].decode("ascii", "replace"))
                pos += 1 + packet[pos]
            name, end = ".".join(labels), pos + 5
            qtype = int.from_bytes(packet[pos + 1:pos + 3], "big")
            asker = self.owner(peer[1])  # while the asking socket still waits for this answer
            with self.lock:
                self.names[f"{asker}: {name}"] += 1
                self.log.write(f"{time.strftime('%H:%M:%S')} {asker} type {qtype} {name}\n")
            flags = 0x8080 | (int.from_bytes(packet[2:4], "big") & 0x0100) | 3
            sock.sendto(packet[:2] + flags.to_bytes(2, "big") + b"\0\1\0\0\0\0\0\0" + packet[12:end], peer)

    @staticmethod
    def owner(port):
        """The process (name, pid) holding this namespace's UDP socket on a local port."""
        inodes = set()
        for table in ("/proc/net/udp", "/proc/net/udp6"):
            for line in Path(table).read_text().splitlines()[1:]:
                fields = line.split()
                if int(fields[1].rsplit(":", 1)[1], 16) == port:
                    inodes.add(f"socket:[{fields[9]}]")
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                for fd in (entry / "fd").iterdir():
                    if os.readlink(fd) in inodes:
                        return f"{(entry / 'comm').read_text().strip()}({entry.name})"
            except OSError:
                continue
        return "unknown"


def stop_group(proc):
    """Stop a process this harness started, with everything it started in its own session."""
    for sig, wait in ((15, 5), (9, 5)):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            break
        try:
            proc.wait(wait)
            break
        except subprocess.TimeoutExpired:
            continue
    try:
        os.killpg(proc.pid, 9)  # children left after the leader exited
    except ProcessLookupError:
        pass


def start_xvfb(stack, work):
    number = next(n for n in range(87, 300)
                  if not Path(f"/tmp/.X11-unix/X{n}").exists() and not Path(f"/tmp/.X{n}-lock").exists())
    log = stack.enter_context((work / "xvfb.log").open("wb"))
    # Abstract socket only: it belongs to this network namespace, so no client outside it can use
    # the display, and a client inside cannot fall through to another X server's file socket.
    proc = subprocess.Popen([shutil.which("Xvfb", path=LINUX_PATH), f":{number}", "-nolisten", "tcp", "-nolisten",
                             "unix", "-screen", "0", "1280x800x24"], stdout=log, stderr=subprocess.STDOUT,
                            env={"PATH": LINUX_PATH}, start_new_session=True)
    stack.callback(stop_group, proc)
    until = time.monotonic() + 15
    while f"@/tmp/.X11-unix/X{number}" not in Path("/proc/net/unix").read_text():
        if proc.poll() is not None or time.monotonic() > until:
            raise RuntimeError("Xvfb did not start")
        time.sleep(.2)
    return f":{number}"


def linux_dolphin(path, work):
    """The stock app as published: an extracted AppImage folder, or the AppImage extracted here."""
    record = []
    if path.is_file():
        record.append(f"AppImage {path} sha256 {hashlib.sha256(path.read_bytes()).hexdigest()}")
        target = work / "dolphin-app"
        target.mkdir()
        subprocess.run([path, "--appimage-extract"], cwd=target, check=True, stdout=subprocess.DEVNULL,
                       env={"PATH": LINUX_PATH})
        root = target / "squashfs-root"
    else:
        root = path
    for name in ("AppRun", "usr/bin/dolphin-emu", "usr/lib/libslippi_rust_extensions.so"):
        record.append(f"{name} sha256 {hashlib.sha256((root / name).read_bytes()).hexdigest()}")
    rust = (root / "usr/lib/libslippi_rust_extensions.so").read_bytes()
    if b"SLIPPI_MM_URL" not in rust:
        raise RuntimeError("This Dolphin's matchmaking client does not read SLIPPI_MM_URL")
    record.append("libslippi_rust_extensions.so contains SLIPPI_MM_URL")
    (work / "stock-dolphin.txt").write_text("\n".join(record) + "\n")
    return root


def prepare_stock_linux(root, work, spectator_port, peer_port, software_rendering=True):
    user = work / "User"
    config = write_stock_config(user, work, spectator_port, peer_port)
    # Linux 3.7.0 reads its account from <User>/Slippi/user.json: here, the fabricated one.
    (user / "Slippi").mkdir()
    (user / "Slippi/user.json").write_text(json.dumps(local_user(1)))
    pad = (root / "usr/bin/Sys/Config/Profiles/GCPad/slippibot.ini").read_text()
    (config / "GCPadNew.ini").write_text(pad.replace("[Profile]", "[GCPad1]"))
    # The Linux 3.7.0 build is Ishiiruka-based and has no Null video backend: `-v Null` falls back
    # to OpenGL, which here is Mesa's software rasterizer. At Slippi's default 2x internal
    # resolution that held the emulation below 60 fps (pace.log), so render at native resolution
    # without anti-aliasing: rendering only, the game and netplay settings are untouched. On a
    # real GPU (--display) only VSync is turned off, as Slippi advises.
    if software_rendering:
        (config / "GFX.ini").write_text("[Hardware]\nVSync = False\n[Settings]\nEFBScale = 2\nMSAA = 1\n"
                                        "SSAA = False\nShowFPS = False\nShowNetPlayPing = False\n")
    else:
        (config / "GFX.ini").write_text("[Hardware]\nVSync = False\n")
    (user / "Pipes").mkdir()
    path = user / "Pipes/slippibot1"
    os.mkfifo(path)
    return path


def start_game_linux(stack, command, work, url, display, extra=None):
    home = work / "home"
    for directory in (home / ".config", home / ".local/share", home / ".cache"):
        directory.mkdir(parents=True, exist_ok=True)
    runtime = work / "run"
    runtime.mkdir(mode=0o700, exist_ok=True)
    # No session bus (xdg-screensaver would otherwise start one) and no accessibility bus.
    env = {"PATH": LINUX_PATH, "HOME": str(home), "XDG_CONFIG_HOME": str(home / ".config"),
           "XDG_DATA_HOME": str(home / ".local/share"), "XDG_CACHE_HOME": str(home / ".cache"),
           "XDG_RUNTIME_DIR": str(runtime), "DISPLAY": display, "NO_PROXY": "*", "SLIPPI_MM_URL": url,
           "DBUS_SESSION_BUS_ADDRESS": "disabled:", "NO_AT_BRIDGE": "1", "LANG": "C.UTF-8", **(extra or {})}
    log = stack.enter_context((work / "console.log").open("wb"))
    proc = subprocess.Popen([str(part) for part in command], cwd=work, stdout=log, stderr=subprocess.STDOUT,
                            env=env, start_new_session=True)
    stack.callback(stop_group, proc)
    return proc


def dolphin_pid(session):
    """The emulator process in a session this harness started (AppRun execs it, strace forks it)."""
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                stat = (entry / "stat").read_text()
            except OSError:
                continue
            comm = stat[stat.index("(") + 1:stat.rindex(")")]
            fields = stat[stat.rindex(")") + 2:].split()
            if int(fields[3]) == session and comm in ("AppRun.wrapped", "dolphin-emu"):
                return int(entry.name)
    return None


class DolphinRam:
    """The emulated console's 24 MB of main memory, read through /proc from Dolphin's shared mapping."""

    def __init__(self, pid):
        self.pid, self.fd, self.base = pid, None, None

    def attach(self):
        try:
            maps = Path(f"/proc/{self.pid}/maps").read_text().splitlines()
            fd = os.open(f"/proc/{self.pid}/mem", os.O_RDONLY)
        except OSError:
            return False
        for line in maps:
            fields = line.split(None, 5)
            start, end = (int(x, 16) for x in fields[0].split("-"))
            if len(fields) < 6 or "dolphinmem" not in fields[5] or int(fields[2], 16) or end - start < 0x1800000:
                continue
            try:
                if os.pread(fd, 6, start) == b"GALE01":
                    self.fd, self.base = fd, start
                    return True
            except OSError:
                continue
        os.close(fd)
        return False

    def read(self, address, size):
        return os.pread(self.fd, size, self.base + (address - 0x80000000))

    def u8(self, address):
        return self.read(address, 1)[0]

    def u16(self, address):
        return int.from_bytes(self.read(address, 2), "big")

    def u32(self, address):
        return int.from_bytes(self.read(address, 4), "big")

    def f32(self, address):
        return struct.unpack(">f", self.read(address, 4))[0]

    def pointer(self, address):
        value = self.u32(address)
        return value if 0x80000000 <= value < 0x81800000 else None


class PaceRecorder:
    """Twice a second: Dolphin's match frame and scene, and the CPU time each game used, with its
    busiest thread, against the machine's idle and steal time. Slow or stalled emulation shows
    here as frames per second below 60 together with whether Dolphin was busy or waiting."""

    def __init__(self, path, ram, pids):
        self.log, self.ram, self.pids, self.last, self.next = path.open("w", buffering=1), ram, pids, None, 0

    @staticmethod
    def threads(pid):
        found = {}
        try:
            for task in Path(f"/proc/{pid}/task").iterdir():
                try:
                    stat = (task / "stat").read_text()
                except OSError:
                    continue
                fields = stat[stat.rindex(")") + 2:].split()
                found[task.name] = (stat[stat.index("(") + 1:stat.rindex(")")], int(fields[11]) + int(fields[12]))
        except OSError:
            pass
        return found

    def sample(self):
        now = time.monotonic()
        if now < self.next:
            return
        self.next = now + .5
        cpu = [int(v) for v in Path("/proc/stat").read_text().split("\n", 1)[0].split()[1:]]
        state = {"t": now, "frame": self.ram.u32(MATCH_FRAMES), "scene": (self.ram.u8(SCENE), self.ram.u8(SCENE + 3)),
                 "cpu": cpu, "threads": {name: self.threads(pid) for name, pid in self.pids.items()}}
        if self.last:
            dt = now - self.last["t"]
            ticks = os.sysconf("SC_CLK_TCK") * dt
            total = sum(cpu) - sum(self.last["cpu"])
            idle = (cpu[3] + cpu[4] - self.last["cpu"][3] - self.last["cpu"][4]) / max(total, 1)
            steal = (cpu[7] - self.last["cpu"][7]) / max(total, 1) if len(cpu) > 7 else 0
            parts = []
            for name, threads in state["threads"].items():
                before = self.last["threads"].get(name, {})
                used = {tid: (comm, t - before[tid][1]) for tid, (comm, t) in threads.items() if tid in before}
                top = max(used.values(), key=lambda item: item[1], default=("-", 0))
                parts.append(f"{name} {100 * sum(u for _, u in used.values()) / ticks:.0f}% "
                             f"(top {top[0]} {100 * top[1] / ticks:.0f}%)")
            frames = state["frame"] - self.last["frame"]
            self.log.write(f"{time.strftime('%H:%M:%S')} scene {state['scene'][0]}:{state['scene'][1]} "
                           f"match frame {state['frame']} ({frames / dt:.1f}/s) {'; '.join(parts)}; "
                           f"machine idle {100 * idle:.0f}% steal {100 * steal:.1f}% load {os.getloadavg()[0]:.2f}\n")
        self.last = state


def setup_progress(text):
    """The fork log's view of the set: (game number, setup steps finished in its setup, results)."""
    overs = list(re.finditer(r"ranked set: game (\d+) over, winner (\w+)", text))
    tail = text[overs[-1].end():] if overs else text
    done = [int(n) for n in re.findall(r"ranked setup: (?:send|receive) step (\d+)", tail)]
    return len(overs) + 1, done, [m.group(2) for m in overs]


class StockBot:
    """Stock Dolphin's controller: libmelee's pipe commands, decided from the console's memory.

    Menus: Online Play, Ranked, a chosen character, Start to search. Ranked setup: the step in
    progress is read from the fork's log (both clients run the same setup steps), and Dolphin's
    own setup steps get a D-pad and A sequence: strikes, a counterpick and a character change.
    Matches: walk to the opponent while keeping clear of the ledges and recover when knocked off.
    Every game is played lightly for --active-seconds; then, to win, Dolphin's fighter attacks
    with forward smashes, and to lose, it walks off the stage. The intent of each game keeps the
    set going to a third game."""

    def __init__(self, pipe, ram, native_log, args, slot=1):
        self.pipe, self.ram, self.native_log, self.slot = pipe, ram, native_log, slot
        self.icon, self.counterpicks, self.active = args.stock_icon, args.stock_counterpicks, args.active_seconds
        self.tick, self.frame, self.busy, self.pending = 0, None, 0, []
        self.scene, self.scene_tick = None, 0
        self.ranked_row, self.row_used = 0, None
        self.native_text, self.native_offset = "", 0
        self.game, self.game_tick, self.intent, self.stocks, self.edge = 0, 0, None, None, 56.0
        self.games = []
        self.attack_ready, self.attack_kind, self.recover_ready = 0, 0, 0
        self.step_key, self.step_since, self.macro_rounds = None, 0, Counter()
        self.character = ICON_CHARACTER[self.icon]  # the picker's index: the external character id
        self.next_move, self.next_report, self.next_refresh = 0, 0, 0
        self.events = []

    def note(self, text):
        line = f"{time.strftime('%H:%M:%S')} stock: {text}"
        self.events.append(line)
        print(line, flush=True)

    def at(self, delay, *lines):
        self.pending.append((self.tick + delay, "\n".join(lines) + "\n"))

    def press(self, button, delay=0, hold=4):
        self.at(delay, f"PRESS {button}")
        self.at(delay + hold, f"RELEASE {button}")

    def neutral(self):
        self.pending.clear()
        self.at(0, *[f"RELEASE {b}" for b in ("A", "B", "X", "Z", "R", "START", "D_UP", "D_DOWN", "D_LEFT", "D_RIGHT")],
                "SET MAIN 0.5 0.5", "SET C 0.5 0.5", "SET R 0")

    def sequence(self, buttons, spacing=12):
        for i, button in enumerate(buttons):
            self.press(button, i * spacing)
        self.busy = self.tick + len(buttons) * spacing + 6

    def flush(self):
        due = [item for item in self.pending if item[0] <= self.tick]
        if due:
            self.pending = [item for item in self.pending if item[0] > self.tick]
            try:
                os.write(self.pipe, "".join(text for _, text in sorted(due, key=lambda item: item[0])).encode())
            except BlockingIOError:
                pass

    def refresh_native(self):
        try:
            with self.native_log.open("rb") as f:
                f.seek(self.native_offset)
                chunk = f.read()
        except OSError:
            return
        self.native_offset += len(chunk)
        self.native_text += chunk.decode(errors="replace")

    def fighter(self, slot):
        block = PLAYERS + slot * 0xE90
        if self.ram.u32(block) != 2:
            return None
        stocks = self.ram.u8(block + 0x8E)
        gobj = self.ram.pointer(block + 0xB0)
        data = self.ram.pointer(gobj + 0x2C) if gobj else None
        if not data or self.ram.u32(data) != gobj:
            return {"stocks": stocks, "x": None}
        return {"stocks": stocks, "x": self.ram.f32(data + 0xB0), "y": self.ram.f32(data + 0xB4),
                "facing": self.ram.f32(data + 0x2C), "percent": self.ram.f32(data + 0x1830)}

    def step(self):
        """Called often; acts once per emulated frame, so timings below are in console frames."""
        frame = self.ram.u32(SCENE_FRAMES)
        if frame == self.frame:
            return False
        delta = frame - self.frame if self.frame is not None else 1
        self.frame = frame
        self.tick += delta if 0 < delta <= 10 else 1  # a new scene restarts the count
        self.flush()
        if self.tick >= self.next_refresh:
            self.next_refresh = self.tick + 30
            self.refresh_native()
        major, minor = self.ram.u8(SCENE), self.ram.u8(SCENE + 3)
        if (major, minor) != self.scene:
            self.changed(self.scene, (major, minor))
            self.scene, self.scene_tick = (major, minor), self.tick
        if self.scene == (ONLINE_MAJOR, GAME_MINOR):
            self.play()
            return
        if self.tick < self.busy:
            return
        if major == 0:
            self.press("START")
            self.busy = self.tick + 40
        elif major == 1:
            self.menu()
        elif self.scene == (ONLINE_MAJOR, CSS_MINOR):
            self.select()
        else:
            self.setup()

    def changed(self, old, new):
        self.note(f"scene {old} -> {new}")
        if new == (ONLINE_MAJOR, GAME_MINOR):
            self.refresh_native()
            game, _, results = setup_progress(self.native_text)
            self.game += 1
            stock, fork = results.count("opponent"), results.count("local")
            # Lose the first game, win the second, lose a third: the set needs three games.
            self.intent = "win" if fork > stock else "lose"
            self.game_tick, self.stocks = self.tick, None
            stages = re.findall(r"match setup teams \d+ stage (\d+)", self.native_text)
            self.edge = STAGE_EDGES.get(int(stages[-1]) if stages else 0, 56.0)
            self.neutral()
            self.note(f"game {self.game} (fork log: game {game}, results {results}): playing to {self.intent}")
        elif old == (ONLINE_MAJOR, GAME_MINOR):
            self.games.append({"game": self.game, "intent": self.intent, "final_stocks": self.stocks})
            self.note(f"game {self.game} left with stocks {self.stocks}")
            self.neutral()
        elif new == (ONLINE_MAJOR, CSS_MINOR):
            mode = self.ram.u8(ONLINE_MODE)
            self.note(f"online character select, mode {mode}, from menu row {self.row_used}")
            if mode != 0 and self.row_used is not None:
                self.ranked_row = (self.row_used - mode) % 4
                self.note(f"not Ranked: backing out, Ranked should be row {self.ranked_row}")
                self.at(0, "PRESS B")
                self.at(100, "RELEASE B")
                self.busy = self.tick + 110

    def menu(self):
        kind, row = self.ram.u8(MENU_FLOW), self.ram.u16(MENU_FLOW + 2)
        if kind == 0:
            self.press("A" if row == 0 else "D_UP")
        elif kind == 8:
            if row == self.ranked_row:
                self.row_used = row
                self.press("A")
            else:
                self.press("D_UP" if row > self.ranked_row else "D_DOWN")
        else:
            self.press("B")
        self.busy = self.tick + 20

    def select(self):
        if self.ram.u8(ONLINE_MODE) != 0:
            return
        hand = self.ram.pointer(CSS_CURSORS)
        coin = self.ram.pointer(CSS_COINS)
        if not hand or not coin:
            return
        holding = self.ram.u8(hand + 5) == 1
        x, y = self.ram.f32(hand + 0xC), self.ram.f32(hand + 0x10)
        icon = self.ram.u8(CSS_DOORS + 0xE)
        if self.tick >= self.next_report:
            self.next_report = self.tick + 120
            self.note(f"select: hand {self.ram.u8(hand + 5)} at {x:.1f},{y:.1f}; coin {self.ram.u8(coin + 5)} at "
                      f"{self.ram.f32(coin + 8):.1f},{self.ram.f32(coin + 0xC):.1f}; icon {icon}")
        if not holding and icon == self.icon:
            self.at(0, "SET MAIN 0.5 0.5")
            self.press("START")
            self.busy = self.tick + 90
            return
        # mnCharSel: a hand inside the icon area (0.2 < y < 22) takes its coin back by itself when
        # no character is chosen; with another character chosen, B takes the coin back.
        if not holding and icon < 25 and 0.2 < y < 22:
            self.at(0, "SET MAIN 0.5 0.5")
            self.press("B", 2)
            self.busy = self.tick + 20
            return
        tx, ty = icon_center(self.icon)
        dx, dy = tx - x, ty - y
        if holding and abs(dx) < 1.0 and abs(dy) < 1.0:
            self.at(0, "SET MAIN 0.5 0.5")
            self.press("A", 2)
            self.busy = self.tick + 20
            return

        def axis(d):
            if abs(d) < 0.6:
                return 0.5
            return 0.5 + max(-0.5, min(0.5, d / 10 if abs(d / 10) > 0.3 else (0.3 if d > 0 else -0.3)))
        self.at(0, f"SET MAIN {axis(dx):.3f} {axis(dy):.3f}")

    def setup(self):
        if self.scene[1] != SETUP_MINOR:
            # Screens between the setup and a match: Start, as libmelee's menu helper does.
            if self.tick - self.scene_tick > 120:
                self.press("START")
                self.busy = self.tick + 120
            return
        game, done, results = setup_progress(self.native_text)
        # Game 1: colour steps 0-1 (skipped without a colour clash), strikes 2-4 by players 0, 1, 0.
        # Later games: ban by the last winner, stage by the loser, winner's then loser's character.
        step = max(done) + 1 if done else (2 if game == 1 else 0)
        stock_won_last = bool(results) and results[-1] == "opponent"
        if game == 1:
            kind, mine = ("color" if step < 2 else "strike"), step == 3
        else:
            kind = ("strike", "stage", "character", "character", None)[min(step, 4)]
            mine = (step in (0, 2)) == stock_won_last
        key = (game, step)
        if key != self.step_key:
            self.step_key, self.step_since = key, self.tick
            return
        # The fork answers its own steps within a few frames (its script presses A all through the
        # setup). Dolphin's steps get a sequence after a second; any step still open after eight
        # seconds gets one too, in case a step's owner differs from the above (Dolphin ignores
        # presses on the other player's step, and every step has a timeout).
        waited = self.tick - self.step_since
        if kind is None or waited < (60 if mine else 480):
            return
        attempt = self.macro_rounds[key]
        self.macro_rounds[key] += 1
        if kind == "strike":
            buttons = ["D_RIGHT", "A"] * (1 + attempt % 2) + ["A"]
        elif kind == "stage":
            buttons = ["D_RIGHT"] * (game - 1) + ["A", "A"]
        elif kind == "character" and attempt == 0 and mine:
            target = self.counterpicks[min(game - 2, len(self.counterpicks) - 1)]
            buttons = ["D_RIGHT", "A"] + picker_path(self.character, target) + ["A", "A"]
            self.character = target
        elif kind == "color":
            buttons = ["D_RIGHT", "A", "A"]
        else:
            buttons = ["A"]
        self.note(f"setup game {game} step {step} ({kind}, {'Dolphin' if mine else 'fork?'}), "
                  f"attempt {attempt + 1}: {' '.join(buttons)}")
        self.sequence(buttons)
        self.busy += 90

    def play(self):
        me, them = self.fighter(self.slot), self.fighter(1 - self.slot)
        if me and them:
            self.stocks = (them["stocks"], me["stocks"]) if self.slot else (me["stocks"], them["stocks"])
            if self.tick >= self.next_report and me["x"] is not None and them["x"] is not None:
                self.next_report = self.tick + 1200
                self.note(f"game {self.game} at {(self.tick - self.game_tick) / 60:.0f} s: fork {them['stocks']} stocks "
                          f"{them['percent']:.0f}% x {them['x']:.0f}; Dolphin {me['stocks']} stocks "
                          f"{me['percent']:.0f}% x {me['x']:.0f}")
        if self.tick < self.next_move or self.tick < self.busy or not me or me["x"] is None:
            return
        self.next_move = self.tick + 3
        seconds = (self.tick - self.game_tick) / 60
        if self.intent == "lose" and seconds > self.active:
            self.at(0, f"SET MAIN {1.0 if me['x'] >= 0 else 0.0} 0.5")  # walk off the nearer side
            return
        edge = self.edge
        inward = 1.0 if me["x"] < 0 else 0.0
        if me["y"] < -6 or (abs(me["x"]) > edge + 2 and me["y"] < 10):
            # Off the stage: toward the middle, jump, up special.
            self.at(0, f"SET MAIN {inward} 1.0")
            if self.tick >= self.recover_ready:
                self.press("X")
                self.press("B", 10)
                self.recover_ready = self.tick + 50
            return
        dy = them["y"] - me["y"] if them and them["x"] is not None else 0
        if me["y"] > 10 and (abs(me["x"]) > edge - 10 or dy < -8):
            # On a platform (or in the air): over the stage first, then drop through to the opponent.
            if abs(me["x"]) > edge - 10:
                self.at(0, f"SET MAIN {0.5 + (0.3 if inward else -0.3)} 0.5")
            else:
                self.at(0, "SET MAIN 0.5 0.0")
                self.at(3, "SET MAIN 0.5 0.5")
                self.next_move = self.tick + 30
            return
        if not them or them["x"] is None or them["y"] < -10 or abs(them["x"]) > edge + 4:
            # The opponent is off the stage: wait near the middle.
            self.at(0, f"SET MAIN {0.7 if inward else 0.3} 0.5" if abs(me["x"]) > 25 else "SET MAIN 0.5 0.5")
            return
        if abs(me["x"]) > edge - 5:  # at a ledge: back toward the middle
            self.at(0, f"SET MAIN {inward} 0.5")
            return
        dx = them["x"] - me["x"]
        # A game to win is played lightly for as long as one to lose, then with forward smashes.
        attacking = self.intent == "win" and seconds > self.active
        if dy > 8 and attacking and abs(dx) < 8 and self.tick >= self.attack_ready:
            self.press("X")  # an opponent on a platform above: jump and up-air
            self.at(8, "SET C 0.5 1.0")
            self.at(14, "SET C 0.5 0.5")
            self.attack_ready = self.tick + 60
            return
        # Playing lightly, keep off a fork near a ledge: it never recovers, and shield pushback or
        # a grab release there puts it off the stage.
        near_ledge = not attacking and abs(them["x"]) > edge - 20
        reach = 12 if attacking else 22 if near_ledge else 6
        if abs(dx) > reach or dy > 8:
            # Run while far, walk when close; stay a fighter's width inside the ledges.
            outward = (dx > 0) == (me["x"] > 0) and abs(me["x"]) > edge - 9
            speed = 0.35 if abs(dx) > reach + 12 else 0.2
            self.at(0, "SET MAIN 0.5 0.5" if outward else f"SET MAIN {0.5 + speed if dx > 0 else 0.5 - speed} 0.5")
            return
        self.at(0, "SET MAIN 0.5 0.5")
        if self.tick < self.attack_ready:
            return
        toward = "1.0" if dx > 0 else "0.0"
        if not attacking and (me["facing"] > 0) != (dx > 0):
            self.at(0, f"SET MAIN {0.65 if dx > 0 else 0.35} 0.5")  # turn to face the opponent
            self.at(3, "SET MAIN 0.5 0.5")
            self.attack_ready = self.tick + 8
            return
        if attacking:
            self.at(0, f"SET C {toward} 0.5")  # forward smash (the C-stick turns around by itself)
            self.at(6, "SET C 0.5 0.5")
            self.attack_ready = self.tick + 55
        else:
            # Moves that do not send the fork (which never recovers) off the stage: a jab while
            # its percent is low, crouching and shielding.
            kind = self.attack_kind % 3
            self.attack_kind += 1
            if kind == 0 and them["percent"] < 80 and not near_ledge:
                self.press("A")  # jab
            elif kind < 2:
                self.at(0, "SET MAIN 0.5 0.0")  # crouch
                self.at(20, "SET MAIN 0.5 0.5")
            else:
                self.at(0, "PRESS R", "SET R 1")  # shield
                self.at(20, "RELEASE R", "SET R 0")
            self.attack_ready = self.tick + 40


def slp_summary(path):
    """Frames, stage, fighters and how each fighter's last recorded frame left them."""
    raw = slp_raw(path)
    end = raw[1] + 1
    sizes = {raw[j]: int.from_bytes(raw[j + 1:j + 3], "big") + 1 for j in range(2, end, 3)}
    offset, start, ending, last, frames = end, None, None, {}, 0
    while offset < len(raw):
        event = raw[offset]
        body = raw[offset:offset + sizes[event]]
        if event == 0x36:
            start = body
        elif event == 0x38 and not body[6]:
            frame = int.from_bytes(body[1:5], "big", signed=True)
            frames = max(frames, frame)
            last[body[5]] = {"stocks": body[0x21], "percent": round(struct.unpack(">f", body[0x16:0x1A])[0], 2),
                             "character": body[7], "frame": frame}
        elif event == 0x39:
            ending = body
        offset += len(body)
    return {"file": path.name, "last_frame": frames,
            "stage": int.from_bytes(start[0x13:0x15], "big") if start else None,
            "characters": [start[0x65 + 0x24 * i] for i in range(2)] if start else None,
            "end_method": ending[1] if ending else None, "players": {str(k): v for k, v in sorted(last.items())}}


async def run_linux(args, work, root):
    native = work / "fork"
    (native / "user").mkdir(parents=True)
    (native / "user/user.json").write_text(json.dumps(local_user(0)))
    (native / "pad.txt").write_text(controller(0, setup_frames=args.frames, match=FORK_MATCH))
    fork_env = {"MELEE_NO_GC_ADAPTER": "1", "SDL_AUDIODRIVER": "dummy", "MELEE_TEST_SCRIPT_PAD_FRAMES": "1"}
    if os.environ.get("LD_LIBRARY_PATH"):  # a fork built against user-local libraries
        fork_env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
    stock = work / "stock"
    stock.mkdir()
    peer_port, spectator_port = two_ports()
    fifo = prepare_stock_linux(root, stock, spectator_port, peer_port, software_rendering=not args.display)
    lookups = LookupRecorder(work / "lookups.log")
    print(f"Lookups recorded at the resolvers {lookups.start()} and 127.0.0.53", flush=True)
    queue = Queue()
    summary = {"work": str(work), "fork": str(args.fork), "dolphin": str(root),
               "stock_gl_cache": str(args.stock_gl_cache or ""), "display": args.display or "private Xvfb"}
    try:
        async with serve(queue.handle, "127.0.0.1", 0) as server:
            url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/v1/queue"
            with ExitStack() as stack:
                # A desktop's display is reached through its socket file, which a network namespace
                # does not hide (the abstract X socket it does), with the desktop user's X cookie.
                display = args.display or start_xvfb(stack, work)
                x_auth = {"XAUTHORITY": os.environ.get("XAUTHORITY") or str(Path.home() / ".Xauthority")} \
                    if args.display else {}
                pipe = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
                stack.callback(os.close, pipe)
                command = [root / "AppRun", "-u", stock / "User", "-b", "-c", "False", "-e", args.iso, "-v",
                           args.stock_video, "--slippi-spectator-port", str(spectator_port), "-a", "HLE"]
                if args.trace_stock_network:
                    command = [shutil.which("strace", path=LINUX_PATH), "-f", "-qq", "--seccomp-bpf", "-e",
                               "trace=socket,connect,bind,sendto,sendmsg,sendmmsg", "-o",
                               work / "stock-network.strace", *command]
                # Mesa's own shader cache (a documented Mesa variable; Dolphin is untouched): kept between
                # runs, it spares the software rasterizer the LLVM compiles that stall the first seconds.
                gl_cache = {"MESA_SHADER_CACHE_DIR": str(args.stock_gl_cache)} if args.stock_gl_cache else {}
                old = start_game_linux(stack, command, stock, url, display, {**gl_cache, **x_auth})
                new = start_game_linux(stack, [args.fork, "--iso", args.iso, "--headless", "--allow-matchmaking",
                                               "--volume", "0", "--slippi-menus", "on", "--user-dir", native / "user",
                                               "--sys-dir", args.fork.parent / "Sys", "--card-dir", native / "cards",
                                               "--replay-dir", native / "games", "--settings-path",
                                               native / "settings.ini", "--script", native / "pad.txt",
                                               "--frames", str(args.frames), "--log-file", native / "host.log"],
                                       native, url, display, {**fork_env, **x_auth})
                until = time.monotonic() + 90
                while True:
                    pid = dolphin_pid(old.pid)
                    if pid and (ram := DolphinRam(pid)).attach():
                        break
                    if old.poll() is not None or time.monotonic() > until:
                        raise RuntimeError("Could not find stock Dolphin's emulated memory")
                    await asyncio.sleep(.5)
                print(f"Reading stock Dolphin's console memory (pid {pid})", flush=True)
                bot = StockBot(pipe, ram, native / "host.log", args)
                pace = PaceRecorder(work / "pace.log", ram, {"dolphin": pid, "fork": new.pid})
                deadline = time.monotonic() + args.timeout
                outcome, next_check = "timed out", 0
                while time.monotonic() < deadline:
                    if old.poll() is not None or new.poll() is not None:
                        outcome = f"a game exited early (stock {old.poll()}, fork {new.poll()})"
                        break
                    bot.step()
                    pace.sample()
                    if time.monotonic() >= next_check:
                        next_check = time.monotonic() + 1
                        bot.refresh_native()
                        if "ranked set: over" in bot.native_text:
                            outcome = "set over"
                            break
                        if "ranked set: abandoned" in bot.native_text or "ranked setup: ended" in bot.native_text:
                            outcome = "set abandoned"
                            break
                    await asyncio.sleep(.004)
                summary["outcome"] = outcome
                summary["stock_bot"] = {"games": bot.games, "events": bot.events}
                await asyncio.sleep(5)  # Let both recorders finalize their files.
    finally:
        summary["queue_joins"] = sorted(queue.joins)
        summary["lookups"] = dict(lookups.names)
        (work / "summary.json").write_text(json.dumps(summary, indent=1))
    check_linux_results(args, work, native, stock, summary)


def trace_destinations(path):
    """'call family address result' -> count, from strace's socket address arguments."""
    seen = Counter()
    for line in path.read_text(errors="replace").splitlines():
        call = re.match(r"\d+\s+(\w+)\(", line)
        family = re.search(r"sa_family=(AF_\w+)", line)
        if not call or not family or call.group(1) not in ("connect", "bind", "sendto", "sendmsg", "sendmmsg"):
            continue
        port = re.search(r"sin6?_port=htons\((\d+)\)", line)
        v4 = re.search(r'sin_addr=inet_addr\("([^"]+)"\)', line)
        v6 = re.search(r'inet_pton\(AF_INET6, "([^"]+)"', line)
        unix = re.search(r'sun_path=(@?"[^"]*")', line)
        if v4:
            address = f"{v4.group(1)}:{port.group(1) if port else '?'}"
        elif v6:
            address = f"[{v6.group(1)}]:{port.group(1) if port else '?'}"
        else:
            address = unix.group(1) if unix else "-"
        results = re.findall(r"\) = (-?\d+)(?: (E[A-Z]+))?", line)
        outcome = (results[-1][1] or "ok") if results else "unfinished"
        seen[f"{call.group(1)} {family.group(1)} {address} {outcome}"] += 1
    return dict(sorted(seen.items()))


# Float fields of a post-frame record (the event code is byte 0): position, facing, percent,
# shield, action frame, misc, self-induced and attack-based speeds, hitlag.
POST_FRAME_FLOATS = (0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x22, 0x2B, 0x35, 0x39, 0x3D, 0x41, 0x45, 0x49)


def unequal_records(native_path, stock_path):
    """Fighter records that differ, split into those where every difference is a float that is
    +0.0 on one side and -0.0 on the other (equal values; the offscreen bit is also allowed) and
    the rest. Returns (signed-zero records, other records, signed-zero records by field offset)."""
    native, _ = fighter_records(native_path)
    stock, _ = fighter_records(stock_path)
    zeros = {b"\0\0\0\0", b"\x80\0\0\0"}
    signed_zero, other, fields = 0, 0, Counter()
    for key, a in native.items():
        b = stock.get(key)
        if b is None or len(a) != len(b):
            other += 1
            continue
        if a == b:
            continue
        zero = {pos for pos in POST_FRAME_FLOATS if a[pos:pos + 4] != b[pos:pos + 4] and {a[pos:pos + 4], b[pos:pos + 4]} == zeros}
        rest = [i for i in range(len(a)) if a[i] != b[i] and not any(p <= i < p + 4 for p in zero)
                and not (i == 0x2A and a[i] ^ b[i] == 0x80)]
        if rest:
            other += 1
        elif zero:
            signed_zero += 1
            fields.update(f"{pos:#x}" for pos in zero)
    return signed_zero, other, dict(fields)


def check_linux_results(args, work, native, stock, summary):
    host = (native / "host.log").read_text(errors="replace")
    stock_log = (stock / "User/Logs/dolphin.log").read_text(errors="replace")
    fork_reports = [tuple(int(v) for v in m) for m in re.findall(
        r"game report: mode 0, (\d+) frames, game (\d+), tiebreak (\d+), winner (-?\d+), end (\d+), lras (-?\d+), stage (\d+)", host)]
    stock_reports = [tuple(int(v) for v in (f, g, t, w, e, l, s)) for f, g, t, w, s, e, l in re.findall(
        r"Frames: (\d+), GameIdx: (\d+), TiebreakIdx: (\d+), WinnerIdx: (-?\d+), StageId: (\d+), GameEndMethod: (\d+), LRASInitiator: (-?\d+)", stock_log)]
    summary["fork_reports"], summary["stock_reports"] = fork_reports, stock_reports
    summary["set"] = re.findall(r"ranked set: (?:game \d+ over.*|over.*|abandoned.*)", host)
    summary["fork_desync_lines"] = [line for line in host.splitlines() if "DESYNC" in line]
    summary["fork_netplay_summaries"] = re.findall(r"netplay summary: .*", host)
    summary["stock_desync_lines"] = [line for line in stock_log.splitlines() if "desync" in line.lower()]
    summary["fork_disconnect_lines"] = [line for line in host.splitlines() if "disconnect" in line.lower()][:40]
    summary["stock_disconnect_lines"] = [line for line in stock_log.splitlines() if "disconnect" in line.lower()][:40]
    native_games = sorted((native / "games").glob("*.slp"))
    stock_games = sorted((stock / "games").glob("*.slp"))
    games, problems, notes = [], [], []
    for i in range(max(len(native_games), len(stock_games))):
        entry = {}
        for side, files in (("fork", native_games), ("stock", stock_games)):
            if i < len(files):
                entry[side] = slp_summary(files[i])
        if "fork" in entry and "stock" in entry:
            try:
                differences = compare_game(native_games[i], stock_games[i])
                entry["fighter_record_differences"] = {f"{pos:#x}/{mask:#x}": n for (pos, mask), n in differences.items()}
                if any(field != (0x2A, 0x80) for field in differences):
                    # A float recorded as +0.0 by one client and -0.0 by the other is the same value:
                    # listed in the summary and the notes, not counted as a state difference.
                    signed_zero, other, fields = unequal_records(native_games[i], stock_games[i])
                    entry["signed_zero_records"], entry["signed_zero_fields"] = signed_zero, fields
                    if other:
                        problems.append(f"game {i + 1}: fighter state differs outside the offscreen bit")
                    else:
                        notes.append(f"game {i + 1}: {signed_zero} fighter records differ only in the sign of a "
                                     f"zero float (fields {fields}); every other byte is identical")
            except ValueError as error:
                entry["comparison_error"] = str(error)
                problems.append(f"game {i + 1}: {error}")
            same = {k: entry["fork"][k] == entry["stock"][k] for k in ("last_frame", "stage", "characters", "end_method", "players")}
            entry["same"] = same
            if not all(same.values()):
                problems.append(f"game {i + 1}: recordings disagree on {[k for k, v in same.items() if not v]}")
        else:
            problems.append(f"game {i + 1}: only one side recorded it")
        games.append(entry)
    summary["games"] = games
    trace = work / "stock-network.strace"
    if trace.exists():
        summary["stock_network_destinations"] = destinations = trace_destinations(trace)
        outside = [d for d in destinations if d.split()[1] in ("AF_INET", "AF_INET6") and not any(
            d.split()[2].startswith(p) for p in ("127.", "[::1]", "[::ffff:127.", "10.255.255.254:53", "0.0.0.0", "[::]"))]
        if outside:
            problems.append(f"Dolphin addressed hosts off loopback: {outside}")
    if summary.get("outcome") != "set over":
        problems.append(f"the set did not finish: {summary.get('outcome')}")
    if len(fork_reports) < args.min_games:
        problems.append(f"{len(fork_reports)} games reported, fewer than {args.min_games}")
    if fork_reports != stock_reports:
        problems.append("the two clients' game reports differ")
    if summary["fork_desync_lines"] or summary["stock_desync_lines"]:
        problems.append("desync reported")
    if any(not re.search(r"desyncs 0 result", line) for line in summary["fork_netplay_summaries"]):
        problems.append("a fork netplay summary counts desyncs")
    if summary.get("queue_joins") != [0, 1]:
        problems.append("the stand-in queue did not see both clients")
    summary["problems"], summary["notes"] = problems, notes
    (work / "summary.json").write_text(json.dumps(summary, indent=1))
    for entry in games:
        for side in ("fork", "stock"):
            if side in entry:
                s = entry[side]
                print(f"{s['file']}: last frame {s['last_frame']}, stage {s['stage']}, characters {s['characters']}, "
                      f"end {s['end_method']}, players {s['players']}", flush=True)
    print(f"Fork reports {fork_reports}\nStock reports {stock_reports}", flush=True)
    for note in notes:
        print(f"Note: {note}", flush=True)
    if problems:
        raise SystemExit("Problems: " + "; ".join(problems) + f" (details in {work / 'summary.json'})")
    print(f"Both clients completed a {len(fork_reports)}-game Ranked set with matching reports and recordings, "
          f"no desync reported. Details in {work / 'summary.json'}", flush=True)


def main_linux(parser, args):
    if not os.environ.get(OUTER_NETNS):
        if args.output and args.output.exists():
            parser.error(f"{args.output} already exists")
        enter_network_namespace()
    args.dolphin, args.fork, args.iso = (path.resolve(strict=True) for path in (args.dolphin, args.fork, args.iso))
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="stock-ranked-"))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    print(f"Profiles and results in {work}", flush=True)
    isolate_network(work)  # before any game, queue or display starts
    root = linux_dolphin(args.dolphin, work)
    if args.stock_gl_cache:
        args.stock_gl_cache = args.stock_gl_cache.resolve()
        args.stock_gl_cache.mkdir(parents=True, exist_ok=True)

    def interrupted(signum, frame):  # an outer timeout: stop the games and the display it started
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    asyncio.run(run_linux(args, work, root))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dolphin", required=True, type=Path,
                        help="macOS: signed Slippi 3.7.0 netplay .app; Linux: the netplay AppImage or its extracted squashfs-root")
    parser.add_argument("--fork", required=True, type=Path,
                        help="macOS: packaged MeleeUnlocked.app; Linux: the melee_source executable (its Sys folder beside it)")
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="New directory for isolated profiles, logs and recordings")
    linux = parser.add_argument_group("Linux only")
    linux.add_argument("--timeout", type=int, default=3600, help="Seconds the set may take (default 3600)")
    linux.add_argument("--frames", type=int, default=230000, help="Frames the fork runs at most (default 230000)")
    linux.add_argument("--min-games", type=int, default=3, help="Games the set must reach (default 3)")
    linux.add_argument("--active-seconds", type=int, default=150,
                       help="Seconds Dolphin's fighter plays lightly before it attacks to win or walks off to lose "
                            "(default 150)")
    linux.add_argument("--stock-icon", type=int, default=7, help="Dolphin's character select icon (default 7, Falcon)")
    linux.add_argument("--stock-counterpicks", type=lambda s: [int(v, 0) for v in s.split(",")], default=[2, 9],
                       help="External character ids Dolphin changes to in games 2, 3 (default 2,9: Fox, Marth)")
    linux.add_argument("--stock-video", default="OGL",
                       help="Dolphin's video backend (default OGL; the Linux 3.7.0 build has no Null backend)")
    linux.add_argument("--trace-stock-network", action="store_true",
                       help="Record Dolphin's connect/sendto/sendmsg/bind calls with strace")
    linux.add_argument("--stock-gl-cache", type=Path,
                       help="Directory kept between runs for Mesa's shader cache (default: a fresh one per run)")
    linux.add_argument("--display",
                       help="An X display with a GPU to render Dolphin on, such as :0 (default: a private Xvfb, "
                            "software rendering at native resolution)")
    args = parser.parse_args()
    if sys.platform.startswith("linux"):
        main_linux(parser, args)
        return
    if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
        parser.error("This harness requires the macOS network sandbox or a Linux network namespace")
    if melee is None:
        parser.error("The macOS harness needs libmelee")
    args.dolphin, args.fork, args.iso = (path.resolve(strict=True) for path in (args.dolphin, args.fork, args.iso))
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="stock-ranked-"))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    print(f"Profiles and results in {work}", flush=True)
    # Dolphin refuses to run on its installer volume. A private copy also lets
    # the sandbox protect the Launcher's real profile when it contains the app.
    with tempfile.TemporaryDirectory(prefix="stock-dolphin-") as directory:
        stock_app = Path(directory) / "Slippi Dolphin.app"
        subprocess.run(["/usr/bin/ditto", args.dolphin, stock_app], check=True)
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", stock_app], check=True)
        asyncio.run(run(args, work, stock_app))


if __name__ == "__main__":
    main()
