"""Run the fork's Ranked peer against the Slippi 3.7 serializer over loopback.

Usage: python3 port/tests/slippi_ranked_peer_interop.py /path/to/Ishiiruka
The pinned Slippi checkout must include Externals/SFML and Source/Core/Core.
This offline test uses Clang on macOS/Linux. It does not start a game or use an account.
"""
import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = "e0e798cb6c8dfa6e8a5565805b6709974284468e"
TEST = ROOT / "port/tests/slippi_ranked_peer_interop.cpp"


def make_upstream_header(checkout, output):
    core = checkout / "Source/Core/Core"
    header = (core / "Slippi/SlippiNetplay.h").read_text()
    source = (core / "Slippi/SlippiNetplay.cpp").read_text()
    proto = (core / "NetPlayProto.h").read_text()
    message_type = next(line for line in proto.splitlines()
                        if "MessageId" in line and ("using" in line or "typedef" in line))
    ids = re.findall(r"NP_MSG_SLIPPI_(?:COMPLETE_STEP|SYNCED_STATE)\s*=\s*0x[0-9A-Fa-f]+", proto)
    assert len(ids) == 2
    records = header[header.index("struct SlippiGamePrepStepResults"):
                     header.index("struct SlippiDesyncRecoveryResp")]
    methods = source[source.index("void SlippiNetplayClient::SendGamePrepStep("):
                     source.index("bool SlippiNetplayClient::GetGamePrepResults(")]
    # Only replace the emulator's asynchronous transport. The records, IDs,
    # serialization methods and SFML implementation all come from this checkout.
    output.write_text(message_type + "\nenum {" + ", ".join(ids) + "};\n" + records + """
struct SlippiNetplayClient {
    u8 playerIdx = 1;
    bool is_desync_recovery = false;
    SlippiSyncedGameState local_sync_state;
    std::unique_ptr<sf::Packet> out;
    void SendAsync(std::unique_ptr<sf::Packet> p) { out = std::move(p); }
    void SendGamePrepStep(SlippiGamePrepStepResults&);
    void SendSyncedGameState(SlippiSyncedGameState&);
};
""" + methods)


def build_and_run(checkout, build):
    def run(args):
        subprocess.run(args, cwd=ROOT, check=True)

    git = ["git", "-C", checkout]
    rev = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
    if rev != REVISION:
        raise SystemExit(f"Expected Ishiiruka {REVISION}, found {rev}")
    run([*git, "diff", "--exit-code", "--quiet", "HEAD", "--", "Source/Core/Core", "Externals/SFML"])
    make_upstream_header(checkout, build / "slippi_stock.inc")

    mac = platform.system() == "Darwin"
    arch = ["-arch", "x86_64"] if mac else []
    cc = [os.getenv("CC", "clang"), *arch]
    cxx = [os.getenv("CXX", "clang++"), *arch, "-std=c++17"]
    objects = []

    def compile_unit(compiler, source, flags, name):
        obj = build / (name + ".o")
        run([*compiler, "-ffunction-sections", "-fdata-sections", *flags, "-c", source, "-o", obj])
        objects.append(obj)

    enet = ROOT / "port/third_party/enet"
    for source in sorted(enet.glob("*.c")):
        if source.stem == "win32":
            continue
        options = ["SOCKLEN_T", "FCNTL", "POLL", "INET_PTON", "INET_NTOP",
                   "GETADDRINFO", "GETNAMEINFO", "MSGHDR_FLAGS"]
        compile_unit(cc, source, ["-I" + str(enet / "include"), *["-DHAS_" + o for o in options]], source.stem)

    sfml = checkout / "Externals/SFML"
    sf_flags = ["-Wno-deprecated-declarations", "-I" + str(sfml / "include")]
    compile_unit(cxx, sfml / "src/SFML/Network/Packet.cpp", [*sf_flags, "-I" + str(sfml / "src")], "sfml_packet")
    compile_unit(cxx, TEST, [*sf_flags, "-DSTOCK_SERIALIZER", "-I" + str(build)], "stock")

    inc = ["linux/compat", "linux", "app", "runtime/ppc", "runtime/hle", "runtime/host",
           "runtime/gx", "runtime/mu_net", "runtime/abi", "third_party", "third_party/enet/include"]
    flags = ["-include", "port/linux/compat/windows.h", "-DMELEE_LINUX", "-DGX_PC_SETTINGS",
             "-DMELEE_SOURCE_PORT", '-DMELEE_PORT_VERSION="interop-test"', *["-Iport/" + p for p in inc]]
    compile_unit(cxx, ROOT / "port/runtime/hle/slippi_net.cpp", flags, "netplay")
    compile_unit(cxx, TEST, flags, "test_driver")
    binary = build / "ranked_peer_test"
    run([*cxx, "-Wl,-dead_strip" if mac else "-Wl,--gc-sections", "-pthread", *objects, "-o", binary])
    run([binary])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkout", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        build_and_run(args.checkout.resolve(), Path(temp))
