"""Detached signatures of fighter plugins (.mufighter), as melee_source checks them (port/app/plugin_gate.h).

A signature is Ed25519 (RFC 8032) over the 40 bytes  b"MUFTSIG1" + SHA-256(plugin file),  stored beside
the plugin as <plugin>.sig (wolf.mufighter -> wolf.mufighter.sig): 128 hex digits and a newline (64 raw
bytes are accepted too). A plugin whose signature verifies against a public key compiled into the
host (port/app/plugin_keys.cpp) loads without asking the player.

    python tools/fighter_plugin_sig.py sign   --seed-file <key file> <plugin.mufighter>...
    python tools/fighter_plugin_sig.py verify --public-key <64 hex | key file> <plugin.mufighter>...
    python tools/fighter_plugin_sig.py public --seed-file <key file>

A key file holds one line of hex (32 bytes); lines starting with # are comments. This script never
reads a key from anywhere but the file it is given. The only key in the repository is the TEST-ONLY one
(port/tests/plugin_test_key), whose private half is public and so proves nothing. Needs the
`cryptography` package.
"""
import argparse
import hashlib
import sys
from pathlib import Path

PREFIX = b"MUFTSIG1"


def message(data: bytes) -> bytes:
    return PREFIX + hashlib.sha256(data).digest()


def key_bytes(text: str) -> bytes:
    lines = [line.strip() for line in text.splitlines() if line.strip() and not line.strip().startswith("#")]
    if len(lines) != 1:
        raise SystemExit("a key file holds one line of hex")
    value = bytes.fromhex(lines[0])
    if len(value) != 32:
        raise SystemExit("a key is 32 bytes (64 hex digits)")
    return value


def private_key(seed_file: Path):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    return Ed25519PrivateKey.from_private_bytes(key_bytes(seed_file.read_text()))


def public_key(value: str):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    path = Path(value)
    raw = key_bytes(path.read_text()) if path.is_file() else bytes.fromhex(value)
    return Ed25519PublicKey.from_public_bytes(raw)


def sig_path(plugin: Path) -> Path:
    return plugin.with_name(plugin.name + ".sig")


def read_signature(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) == 64:
        return data
    return bytes.fromhex(data.decode("ascii").strip())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    s = sub.add_parser("sign")
    s.add_argument("--seed-file", type=Path, required=True)
    s.add_argument("plugins", type=Path, nargs="+")
    v = sub.add_parser("verify")
    v.add_argument("--public-key", required=True)
    v.add_argument("plugins", type=Path, nargs="+")
    p = sub.add_parser("public")
    p.add_argument("--seed-file", type=Path, required=True)
    args = ap.parse_args(argv)
    if args.command == "public":
        from cryptography.hazmat.primitives import serialization
        raw = private_key(args.seed_file).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        print(raw.hex())
        return 0
    if args.command == "sign":
        key = private_key(args.seed_file)
        for plugin in args.plugins:
            signature = key.sign(message(plugin.read_bytes()))
            sig_path(plugin).write_text(signature.hex() + "\n", encoding="ascii", newline="\n")
            print(f"{sig_path(plugin)}: signed sha256 {hashlib.sha256(plugin.read_bytes()).hexdigest()}")
        return 0
    from cryptography.exceptions import InvalidSignature
    key = public_key(args.public_key)
    bad = 0
    for plugin in args.plugins:
        try:
            key.verify(read_signature(sig_path(plugin)), message(plugin.read_bytes()))
            print(f"{plugin}: valid")
        except (InvalidSignature, ValueError, FileNotFoundError) as error:
            print(f"{plugin}: NOT valid ({type(error).__name__})")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
