"""Fetch the prebuilt graphics libraries into port/third_party/ (not in the repository).

    dxcompiler.dll, dxil.dll     Microsoft DirectX Shader Compiler 1.9.2609 (x64), which librashader uses
    librashader.dll              librashader 0.12.0 (x86_64 Windows, optimized)
    libdxcompiler.so             the same DirectX Shader Compiler release for Linux (--dxc-linux), which
                                 the Linux Vulkan renderer compiles its shaders with

Their licences and headers are in the repository; the libraries are not. This downloads each
vendor's release archive from GitHub at a pinned version, checks it against the SHA-256 GitHub
publishes for it, and extracts each library to the path the build copies it from, after checking the
library against its own pinned SHA-256 (the same bytes the repository used to carry). A library
already in place with the pinned hash is left alone; any mismatch stops with an error. No
credentials needed.

    py -3.12 tools/fetch_graphics_dlls.py [--dxc] [--librashader] [--dxc-linux]
    (all by default; --dxc-linux is part of the default only when run on Linux)
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

THIRD_PARTY = Path(__file__).resolve().parents[1] / "port/third_party"
# name: (archive URL, archive SHA-256, [(member in the archive, destination, DLL SHA-256), ...])
PACKAGES = {
    "dxc": ("https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip",
            "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1",
            [("bin/x64/dxcompiler.dll", "dxc/bin/dxcompiler.dll",
              "0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59"),
             ("bin/x64/dxil.dll", "dxc/bin/dxil.dll",
              "64be2368211d257a038a7d4ea6bb3e24c878eb8d588c7b8414fccf759b69c2ac")]),
    "librashader": ("https://github.com/SnowflakePowered/librashader/releases/download/librashader-v0.12.0/"
                    "librashader-x86_64-windows-v0.12.0-optimized.zip",
                    "521fe0f364bfa705883f9e99fb1733a3a24f0f403bcc5591b6b78f6cff289183",
                    [("librashader.dll", "librashader/bin/librashader.dll",
                      "1890f647c7fbe52d4cc591526db24367caca284996855c4565c6003c7e46f8cc")]),
    "dxc-linux": ("https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/"
                  "linux_dxc_2026_09_28.x86_x64.tar.gz",
                  "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1",
                  [("lib/libdxcompiler.so", "dxc/linux/libdxcompiler.so",
                    "d40e4a7b3b46490313f4bfd08101946483a673831b3ea65c93d746de1c2f9216")]),
}
# Only fetched by default where it is used.
LINUX_ONLY = {"dxc-linux"}


def archive_members(name: str, data: bytes, wanted: set[str]) -> dict[str, bytes]:
    """The wanted files of a .zip or .tar.gz archive, read in memory (nothing is unpacked to disk)."""
    found = {}
    if name.endswith(".tar.gz"):
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            for info in t.getmembers():
                member = info.name.removeprefix("./")   # DXC's Linux archive stores ./lib/...
                if member in wanted and info.isfile():
                    found[member] = t.extractfile(info).read()
        return found
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        # DXC's archive stores Windows paths (bin\x64\...); zipfile turns those into / only on Windows.
        for info in z.infolist():
            member = info.filename.replace("\\", "/")
            if member in wanted:
                found[member] = z.read(info)
    return found


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(kind: str) -> None:
    url, archive_sha, members = PACKAGES[kind]
    stale = [(m, dest, sha) for m, dest, sha in members
             if not (THIRD_PARTY / dest).is_file() or sha256_file(THIRD_PARTY / dest) != sha]
    if not stale:
        print(f"{kind}: up to date")
        return
    name = url.rsplit("/", 1)[1]
    data = urllib.request.urlopen(url, timeout=600).read()
    if hashlib.sha256(data).hexdigest() != archive_sha:
        raise SystemExit(f"{name}: SHA-256 does not match the published release")
    entries = archive_members(name, data, {member for member, _, _ in stale})
    libs = []
    for member, dest, sha in stale:
        if member not in entries:
            raise SystemExit(f"{name}: {member} is not in the archive")
        lib = entries[member]
        if hashlib.sha256(lib).hexdigest() != sha:
            raise SystemExit(f"{name}: {member} does not match the pinned SHA-256")
        libs.append((THIRD_PARTY / dest, lib))
    for target, lib in libs:   # written only once every file of the archive checked out
        replaced = " (replaced a different file)" if target.exists() else ""
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(target.name + ".part")
        try:
            partial.write_bytes(lib)
            os.replace(partial, target)
        finally:
            partial.unlink(missing_ok=True)
        print(f"{target.relative_to(THIRD_PARTY.parents[1]).as_posix()}: {len(lib) / 1e6:.1f} MB from {name}{replaced}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for kind in PACKAGES:
        ap.add_argument(f"--{kind}", action="store_true")
    args = ap.parse_args()
    kinds = [k for k in PACKAGES if getattr(args, k.replace("-", "_"))] or \
            [k for k in PACKAGES if k not in LINUX_ONLY or sys.platform == "linux"]
    for kind in kinds:
        fetch(kind)
    return 0


if __name__ == "__main__":
    sys.exit(main())
