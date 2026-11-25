"""windows-build/client_release.py: the manifest it writes, the zip, the checks on the packaged program,
and --verify (a changed file, a missing file and an extra file are each reported).

    python windows-build/client_release_test.py
"""
import argparse
import contextlib
import hashlib
import io
import json
import re
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import client_release as cr  # noqa: E402

INFO = {"version": "9.9.9", "engine": "source", "plugin_format": 2, "plugin_abi": "5893c5d6ceb0ce7ae2ed556efdcfba98",
        "plugin_signing_keys": 0, "plugin_test_key": False, "public_build": True, "game_public_build": True}


def fake_runner(info, version="9.9.9"):
    def run(exe, args, cwd):
        if args == ["--version"]:
            return 0, version + "\n"
        answer = dict(info)
        answer["plugin_approvals"] = str(cwd / "plugin-approvals.json")
        answer["plugins_folder"] = str(cwd / "fighters").replace("\\", "/")
        return 0, json.dumps(answer) + "\n"
    return run


def small_release(root: Path) -> Path:
    folder = root / "NucleusClient-9.9.9-win64"
    (folder / "Sys/GC").mkdir(parents=True)
    (folder / "melee_source.exe").write_bytes(b"MZ host" * 100)
    (folder / "melee_game.dll").write_bytes(b"MZ game" * 100)
    (folder / "Sys/GC/dsp_coef.bin").write_bytes(bytes(range(256)))
    manifest = {"format": cr.MANIFEST_FORMAT, "kind": cr.KIND, "version": "9.9.9", "git_commit": "0" * 40,
                "files": cr.file_entries(folder)}
    cr.write_json(folder / cr.MANIFEST_NAME, manifest)
    return folder


class Verify(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.folder = small_release(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def test_unchanged_folder_passes(self):
        code, report = cr.verify_folder(self.folder)
        self.assertEqual(code, 0, report)
        self.assertEqual((report["checked"], report["changed"], report["missing"], report["extra"]), (3, [], [], []))

    def test_changed_byte_is_reported(self):
        data = bytearray((self.folder / "melee_game.dll").read_bytes())
        data[10] ^= 1
        (self.folder / "melee_game.dll").write_bytes(bytes(data))
        code, report = cr.verify_folder(self.folder)
        self.assertEqual(code, 1)
        self.assertEqual([c["path"] for c in report["changed"]], ["melee_game.dll"])
        self.assertEqual(report["changed"][0]["sha256"], hashlib.sha256(data).hexdigest())

    def test_changed_size_is_reported(self):
        with (self.folder / "Sys/GC/dsp_coef.bin").open("ab") as f:
            f.write(b"x")
        code, report = cr.verify_folder(self.folder)
        self.assertEqual(code, 1)
        self.assertEqual(report["changed"][0]["path"], "Sys/GC/dsp_coef.bin")
        self.assertEqual(report["changed"][0]["size"], 257)

    def test_missing_file_is_reported(self):
        (self.folder / "melee_source.exe").unlink()
        code, report = cr.verify_folder(self.folder)
        self.assertEqual(code, 1)
        self.assertEqual(report["missing"], ["melee_source.exe"])

    def test_extra_file_is_reported_and_fails_only_when_strict(self):
        (self.folder / "port-settings.ini").write_text("[video]\n")
        code, report = cr.verify_folder(self.folder)
        self.assertEqual((code, report["extra"], report["ok"]), (0, ["port-settings.ini"], True))
        code, report = cr.verify_folder(self.folder, strict=True)
        self.assertEqual((code, report["ok"]), (1, False))

    def test_separate_manifest_is_the_reference(self):
        outside = self.root / "trusted.json"
        shutil.copy(self.folder / cr.MANIFEST_NAME, outside)
        (self.folder / "melee_game.dll").write_bytes(b"replaced")
        # Whoever replaced a file can rewrite the manifest beside it; the trusted copy still catches it.
        manifest = json.loads((self.folder / cr.MANIFEST_NAME).read_text())
        manifest["files"] = cr.file_entries(self.folder)
        cr.write_json(self.folder / cr.MANIFEST_NAME, manifest)
        self.assertEqual(cr.verify_folder(self.folder)[0], 0)
        code, report = cr.verify_folder(self.folder, outside)
        self.assertEqual(code, 1)
        self.assertEqual([c["path"] for c in report["changed"]], ["melee_game.dll"])

    def test_unusable_manifests(self):
        path = self.folder / cr.MANIFEST_NAME
        good = json.loads(path.read_text())
        for files in ([{"path": "../outside.txt", "size": 1, "sha256": "0" * 64}],
                      [{"path": "C:/Windows/x", "size": 1, "sha256": "0" * 64}],
                      [{"path": "a\\b", "size": 1, "sha256": "0" * 64}],
                      [{"path": "a", "size": 1, "sha256": "nothex"}],
                      [{"path": "a", "size": 1, "sha256": "0" * 64}, {"path": "a", "size": 1, "sha256": "0" * 64}],
                      [{"path": "a"}]):
            cr.write_json(path, dict(good, files=files))
            code, report = cr.verify_folder(self.folder)
            self.assertEqual(code, 2, files)
            self.assertIn("error", report)
        cr.write_json(path, dict(good, kind="something else"))
        self.assertEqual(cr.verify_folder(self.folder)[0], 2)
        path.write_text("{not json")
        self.assertEqual(cr.verify_folder(self.folder)[0], 2)
        path.unlink()
        self.assertEqual(cr.verify_folder(self.folder)[0], 2)

    def test_command_line(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cr.main(["--verify", str(self.folder)])
        report = json.loads(out.getvalue())
        self.assertEqual((code, report["ok"], report["action"]), (0, True, "verify"))
        (self.folder / "melee_source.exe").unlink()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cr.main(["--verify", str(self.folder)])
        self.assertEqual((code, json.loads(out.getvalue())["missing"]), (1, ["melee_source.exe"]))


class Probe(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name) / "rel"
        self.folder.mkdir()
        (self.folder / "melee_source.exe").write_bytes(b"MZ")

    def tearDown(self):
        self.temp.cleanup()

    def test_answer_is_kept_without_this_machines_paths(self):
        info = cr.probe(self.folder, "9.9.9", fake_runner(INFO))
        self.assertEqual(info["plugin_abi"], INFO["plugin_abi"])
        self.assertEqual(info["plugin_approvals"], "<release>\\plugin-approvals.json")
        self.assertEqual(info["plugins_folder"], "<release>/fighters")
        self.assertNotIn(self.temp.name, json.dumps(info))

    def test_refusals(self):
        cases = {"TEST-ONLY": dict(INFO, plugin_test_key=True),
                 "no fighter plugin approval": {k: v for k, v in INFO.items() if k != "plugin_signing_keys"},
                 "plugin ABI": dict(INFO, plugin_abi=None),
                 "not the Source Port": dict(INFO, engine="recomp"),
                 # NUCLEUS_PUBLIC_BUILD: a development build of either half is never packaged.
                 "melee_source.exe is not a public build": dict(INFO, public_build=False),
                 "-DNUCLEUS_PUBLIC_BUILD=ON": {k: v for k, v in INFO.items() if k != "public_build"},
                 "melee_game.dll is not a public build": dict(INFO, game_public_build=False),
                 "game_public_build is not true": dict(INFO, game_public_build=None)}
        for words, info in cases.items():
            with self.assertRaises(cr.ReleaseError) as caught:
                cr.probe(self.folder, "9.9.9", fake_runner(info))
            self.assertIn(words, str(caught.exception.code))
        with self.assertRaises(cr.ReleaseError):
            cr.probe(self.folder, "9.9.9", fake_runner(INFO, version="1.0.0"))

    def test_a_probe_that_writes_files_is_refused(self):
        def writes(exe, args, cwd):
            if args == ["--print-info"]:
                (cwd / "melee_port.log").write_text("log")
            return fake_runner(INFO)(exe, args, cwd)
        with self.assertRaises(cr.ReleaseError):
            cr.probe(self.folder, "9.9.9", writes)


class Licences(unittest.TestCase):
    def test_same_licence_texts_as_the_full_release(self):
        source = (cr.ROOT / "tools/package_release.py").read_text(encoding="utf-8")
        block = source[source.index("for src, dst in ((ROOT / \"LICENSE\""):]
        block = block[:block.index("if not src.is_file()")]
        pairs = re.findall(r'\(ROOT / "([^"]+)",\s*"([^"]+)"\)', block)
        self.assertGreater(len(pairs), 20)
        self.assertEqual(sorted(pairs), sorted(cr.LICENSES))


class Package(unittest.TestCase):
    """The whole run on stand-in build outputs: the folder, the manifest inside and beside it, the zip."""

    def test_package(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            host, game, vc, third = root / "host", root / "game", root / "vc", root / "third"
            for d in (host, game, vc, third):
                d.mkdir()
            (host / "melee_source.exe").write_bytes(b"MZ host")
            for name in cr.GAME_FILES + ("melee_game.dbg",):
                (game / name).write_bytes(name.encode())
            (vc / "vcruntime140.dll").write_bytes(b"MZ vc")
            graphics = {}
            for name in ("dxcompiler.dll", "dxil.dll", "librashader.dll"):
                graphics[name] = third / name
                graphics[name].write_bytes(name.encode())
            args = argparse.Namespace(version="9.9.9", allow_dirty=True, host_build=None, game_build=None,
                                      host_dir=host, game_dir=game, no_build=True, cmake="cmake", jobs=1,
                                      out=root / "out", vc_runtime=vc, require_dlss=False)
            with mock.patch.object(cr, "fetch_graphics", return_value=graphics), contextlib.redirect_stdout(io.StringIO()):
                outer = cr.package(args, fake_runner(INFO))
            name = "NucleusClient-9.9.9-win64"
            folder = root / "out" / name
            inner = json.loads((folder / cr.MANIFEST_NAME).read_text())
            self.assertEqual((inner["version"], inner["plugin_abi"], inner["plugin_format"]), ("9.9.9", INFO["plugin_abi"], 2))
            self.assertEqual((inner["kind"], inner["name"]), ("nucleus-client", name))
            self.assertIs(inner["print_info"]["public_build"], True)
            self.assertRegex(inner["git_commit"], r"^[0-9a-f]{40}$")
            self.assertIsInstance(inner["git_dirty"], bool)
            self.assertEqual(inner["print_info"]["engine"], "source")
            self.assertTrue(any("DLSS" in n for n in inner["notes"]))
            paths = [f["path"] for f in inner["files"]]
            for expected in ("melee_source.exe", "melee_game.dll", "melee_game.snapexcl", "melee_game.syms",
                             "dxcompiler.dll", "librashader.dll", "vcruntime140.dll", "Sys/GC/dsp_coef.bin",
                             "SysPlayback/codehandler.bin", "deltas/manifest.json", "Shaders/crt-shaders.pak",
                             "licenses/COPYING.txt", "licenses/monocypher.md", "README.txt"):
                self.assertIn(expected, paths)
            # No batch file (it fell back to the removed melee_port.exe): melee_source.exe starts by itself.
            for absent in ("MeleeUnlockedLauncher.exe", "melee_port.exe", "melee_game.dbg", "MeleeUnlocked.bat",
                           cr.MANIFEST_NAME):
                self.assertNotIn(absent, paths)
            readme = (folder / "README.txt").read_text(encoding="utf-8")
            self.assertNotIn("F1", readme)
            self.assertNotIn(".bat", readme)
            self.assertIn("Escape closes the game", readme)
            self.assertIn("Melee Unlocked", readme)   # the upstream credit stays
            self.assertEqual(paths, sorted(paths))
            for entry in inner["files"][:20]:
                data = (folder / entry["path"]).read_bytes()
                self.assertEqual((entry["size"], entry["sha256"]), (len(data), hashlib.sha256(data).hexdigest()))
            self.assertTrue((root / "out" / f"{name}-symbols" / "melee_game.dbg").is_file())
            # The manifest beside the zip is the inner one plus the zip's own size and hash.
            archive = root / "out" / f"{name}.zip"
            self.assertEqual(outer["zip"], {"name": archive.name, "size": archive.stat().st_size,
                                            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()})
            beside = json.loads((root / "out" / f"{name}.manifest.json").read_text())
            self.assertEqual({k: v for k, v in beside.items() if k != "zip"}, inner)
            self.assertEqual(cr.verify_folder(folder)[0], 0)
            # What a downloader gets: the zip, unpacked, checks out against the manifest beside it.
            unpacked = root / "unpacked"
            with zipfile.ZipFile(archive) as z:
                self.assertTrue(all(n.startswith(name + "/") for n in z.namelist()))
                z.extractall(unpacked)
            code, report = cr.verify_folder(unpacked / name, root / "out" / f"{name}.manifest.json", strict=True)
            self.assertEqual(code, 0, report)
            # A test-key host is never packaged, nor a build without NUCLEUS_PUBLIC_BUILD.
            for refused in (dict(INFO, plugin_test_key=True), dict(INFO, public_build=False),
                            dict(INFO, game_public_build=False)):
                with mock.patch.object(cr, "fetch_graphics", return_value=graphics), contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(cr.ReleaseError):
                        cr.package(args, fake_runner(refused))


if __name__ == "__main__":
    unittest.main()
