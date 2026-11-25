"""Fighter plugin approval through the built program: --list-plugins, --approve-plugin, --revoke-plugin and
the plugin fields of --print-info (port/app/plugin_gate.h, docs/fighter-plugins.md).

    python port/tests/plugin_approval_test.py <path to melee_source.exe>

Each command runs the way a launcher runs it (stdin closed, output on a pipe, print_info_test.run: no
window, no crash report, done within 20 s). Two small plugin objects are made in a folder of the test's
own, with a settings path there, so nothing of an install is read or changed. Checked: both plugins
start blocked; approving one changes only that one; one flipped byte of the approved file blocks it
again; a revoke turns an approval into a refusal; bad command lines exit 2 and a file that is not a
plugin exits 1; a signature by the TEST-ONLY key verifies only in a build configured with
-DMELEE_PLUGIN_TEST_KEY=ON and leaves the plugin blocked in any other."""
import hashlib
import json
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from print_info_test import run  # noqa: E402

SEED = Path(__file__).resolve().parent / "plugin_test_key" / "TEST_ONLY_ed25519_seed.txt"


def plugin_object(name: bytes, salt: int) -> bytes:
    """A 64-bit COFF object with a MuFighterPlugin record whose fighter's m-ex name is `name`."""
    data = bytearray(112)
    struct.pack_into("<II", data, 0, 0x5446554D, 2)
    data[8] = salt
    struct.pack_into("<Q", data, 24, 40)          # fighters -> the entry
    struct.pack_into("<I", data, 32, 1)
    struct.pack_into("<QQ", data, 40, 56, 72)     # entry: descriptor, mex_name
    struct.pack_into("<Q", data, 56, 72)          # descriptor.name
    data[72:72 + len(name) + 1] = name + b"\0"
    relocs = [24, 40, 48, 56]
    data_at, reloc_at = 60, 60 + len(data)
    sym_at = reloc_at + 10 * len(relocs)
    head = struct.pack("<HHIIIHH", 0x8664, 1, 0, sym_at, 2, 0, 0)
    section = b".rdata\0\0" + struct.pack("<IIIIIIHHI", 0, 0, len(data), data_at, reloc_at, 0, len(relocs), 0, 0x40000040)
    rel = b"".join(struct.pack("<IIH", at, 0, 1) for at in relocs)
    symbol_name = b"mu_fighter_plugin\0"
    syms = b".rdata\0\0" + struct.pack("<IhHBB", 0, 1, 0, 3, 0)
    syms += struct.pack("<II", 0, 4) + struct.pack("<IhHBB", 0, 1, 0, 2, 0)
    strings = struct.pack("<I", 4 + len(symbol_name)) + symbol_name
    return head + section + bytes(data) + rel + syms + strings


def one_json(text):
    lines = [line for line in text.splitlines() if line.strip()]
    assert len(lines) == 1, ("one line of output expected", text[:400])
    return json.loads(lines[0])


def main():
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        fighters = root / "fighters"
        fighters.mkdir()
        settings = root / "port-settings.ini"
        wolf = fighters / "wolf.mufighter"
        shulk = fighters / "shulk.mufighter"
        wolf.write_bytes(plugin_object(b"Wolf", 1))
        shulk.write_bytes(plugin_object(b"Shulk", 2))
        (root / "notes.mufighter").write_text("not a plugin " * 10)
        wolf_sha = hashlib.sha256(wolf.read_bytes()).hexdigest()
        shulk_sha = hashlib.sha256(shulk.read_bytes()).hexdigest()
        base = ["--settings-path", str(settings), "--fighters-dir", str(fighters)]

        def cli(*args, expect=0):
            code, out, err, _ = run(exe, list(args), None)
            assert code == expect, (args, code, out, err)
            return one_json(out)

        def by_file(listing):
            return {p["file"]: p for p in listing["plugins"]}

        listing = cli(*base, "--list-plugins")
        assert listing["ok"] and listing["action"] == "list" and listing["folder"] == str(fighters), listing
        assert Path(listing["approvals_file"]) == root / "plugin-approvals.json", listing
        plugins = by_file(listing)
        assert set(plugins) == {"wolf.mufighter", "shulk.mufighter"}, plugins
        assert plugins["wolf.mufighter"]["sha256"] == wolf_sha and plugins["wolf.mufighter"]["name"] == "Wolf", plugins
        assert all(p["status"] == "blocked" and p["approval"] == "none" and p["signature"] == "none" for p in plugins.values())
        test_key = listing["test_key"]
        print(f"list: 2 plugins, both blocked (signing keys {listing['signing_keys']}, test key {test_key})")

        code, out, err, _ = run(exe, ["--print-info"] + base, None)
        info = one_json(out)
        assert code == 0 and info["plugins_folder"] == str(fighters), info
        assert sorted(b["file"] for b in info["plugins_blocked"]) == ["shulk.mufighter", "wolf.mufighter"], info
        assert all(b["approval"] == "none" for b in info["plugins_blocked"]), info
        assert info["plugin_test_key"] is test_key and isinstance(info["plugin_signing_keys"], int), info
        print("print-info: both listed in plugins_blocked")

        approved = cli("--settings-path", str(settings), "--approve-plugin", str(wolf))
        assert approved["ok"] and approved["plugin"]["sha256"] == wolf_sha and approved["plugin"]["status"] == "approved", approved
        store = json.loads((root / "plugin-approvals.json").read_text())
        assert store["format"] == 1 and store["plugins"][wolf_sha]["approved"] is True, store
        assert store["plugins"][wolf_sha]["name"] == "Wolf" and store["plugins"][wolf_sha]["file"] == "wolf.mufighter", store
        plugins = by_file(cli(*base, "--list-plugins"))
        assert plugins["wolf.mufighter"]["status"] == "approved" and plugins["shulk.mufighter"]["status"] == "blocked", plugins
        code, out, _, _ = run(exe, ["--print-info"] + base, None)
        assert [b["file"] for b in one_json(out)["plugins_blocked"]] == ["shulk.mufighter"], out
        print("approve: wolf approved, shulk still blocked")

        flipped = bytearray(wolf.read_bytes())
        flipped[len(flipped) // 2] ^= 1
        wolf.write_bytes(bytes(flipped))
        plugins = by_file(cli(*base, "--list-plugins"))
        assert plugins["wolf.mufighter"]["status"] == "blocked" and plugins["wolf.mufighter"]["approval"] == "none", plugins
        print("one flipped byte: wolf blocked again")

        revoked = cli("--settings-path", str(settings), "--revoke-plugin", wolf_sha.upper())
        assert revoked["ok"] and revoked["was"] == "approved" and revoked["sha256"] == wolf_sha, revoked
        store = json.loads((root / "plugin-approvals.json").read_text())
        assert store["plugins"][wolf_sha]["approved"] is False, store
        print("revoke: the approval is now a refusal")

        assert not cli("--settings-path", str(settings), "--revoke-plugin", "1234", expect=2)["ok"]
        assert not cli("--settings-path", str(settings), "--approve-plugin", expect=2)["ok"]
        assert not cli("--list-plugins", "--revoke-plugin", wolf_sha, expect=2)["ok"]
        refused = cli("--settings-path", str(settings), "--approve-plugin", str(root / "notes.mufighter"), expect=1)
        assert "not a fighter plugin" in refused["error"], refused
        none = cli("--settings-path", str(settings), "--fighters-dir", "none", "--list-plugins")
        assert none["folder"] is None and none["plugins"] == [], none
        print("bad command lines: exit 2; not a plugin: exit 1; --fighters-dir none: no plugins")

        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        except ImportError:
            print("signature: skipped (no cryptography package)")
            return 0
        seed = next(line for line in SEED.read_text().splitlines() if line and not line.startswith("#"))
        key = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed))
        signature = key.sign(b"MUFTSIG1" + hashlib.sha256(shulk.read_bytes()).digest())
        (fighters / "shulk.mufighter.sig").write_text(signature.hex() + "\n")
        plugins = by_file(cli(*base, "--list-plugins"))
        if test_key:
            assert plugins["shulk.mufighter"]["signature"] == "valid" and plugins["shulk.mufighter"]["status"] == "signed", plugins
            print("signature: valid with the test key built in, shulk signed")
        else:
            assert plugins["shulk.mufighter"]["signature"] == "invalid" and plugins["shulk.mufighter"]["status"] == "blocked", plugins
            print("signature: this build trusts no key, so the test signature does not verify and shulk stays blocked")
        bad = bytearray(signature)
        bad[5] ^= 1
        (fighters / "shulk.mufighter.sig").write_text(bytes(bad).hex() + "\n")
        plugins = by_file(cli(*base, "--list-plugins"))
        assert plugins["shulk.mufighter"]["signature"] == "invalid" and plugins["shulk.mufighter"]["status"] == "blocked", plugins
        print("bad signature: blocked, needs approval")
    print("plugin_approval_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
