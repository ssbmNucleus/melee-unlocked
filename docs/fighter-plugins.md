# Fighter plugins: signatures and approval

A fighter plugin (`*.mufighter`) is a fighter's native code. The game library
(`melee_game.dll`) links it into itself and runs it, so a plugin runs on the
player's PC with the same rights as the game. `melee_source.exe` therefore hands
the game library only plugins that are either

- **signed**: a detached signature beside the plugin verifies against a public key
  compiled into this build (SSBM Nucleus's key once the owner adds it), or
- **approved**: the player approved this exact file (its SHA-256) on this PC.

Anything else is **blocked**: the plugin is not loaded and its fighter is not
available this run. The log says so, for example

```
plugins: wolf.mufighter (Wolf, sha256 1f0c2d...): blocked, not approved; its fighter is unavailable this run (melee_source --approve-plugin "C:\...\fighters\wolf.mufighter" approves this file)
```

A blocked plugin never stops the game from starting and never crashes it.
Changing one byte of an approved plugin makes it a different file: it is blocked
again until it is approved again.

Vanilla starts (`--vanilla-only`, `--vanilla-game`) and `--fighters-dir none` load no
plugins, as before. The online plugin digest (each plugin's file name and contents)
is unchanged: the files the game library reads carry the same names and bytes.

The gate is in `port/app/plugin_gate.{h,cpp}`. It is Windows only for now; other
platforms keep the game library's own folder rule.

## How it is enforced

The game library asks the host which folder to load plugins from
(`CMD_MOD_BROWSER` sub-command 6). On Windows the host now always answers with a
folder of its own for this run:

    <folder of the settings file>\plugins-run\<process id>\

(`%TEMP%\MeleeUnlocked-plugins-run\<process id>\` when that path is too long or not
representable in the system code page). It fills that folder from the real plugin
folder (`--fighters-dir`, else `MELEE_FIGHTER_PLUGINS`, else `fighters` beside the
game library) with only the plugins that pass:

1. the plugin is read once, whole, into memory (other writers shut out while it is
   read) and its SHA-256, signature and approval are checked on those bytes;
2. the copy is written from those same bytes;
3. the copy is opened again with writes and deletes shut out (`FILE_SHARE_READ`),
   compared byte for byte with what was checked, and kept open like that until the
   program exits.

So there is no gap between the check and the load: the file the game library opens
is the file that was checked, and nothing can change it while the game runs. The
folder is removed when the program ends; folders left by programs that are no
longer running are removed at the next start.

The game library rescans between major modes; each rescan goes through the gate
again, so a plugin approved while the game runs (for example with
`--approve-plugin` from another window) is picked up at the next rescan. A plugin
already handed over stays loaded until the program ends, even if it is revoked.

## Approving

**Interactive start** (no `--hidden`, `--headless`, `--no-dialogs` or automated
test environment): before the game starts, each plugin nobody has decided on yet is
put to the player as a plain yes/no message box naming the fighter, the file and the
start of its SHA-256, and saying that a plugin is program code that runs on this PC.
"No" is the default button. The answer is kept for that exact file: "yes" approves
it, "no" records a refusal and the player is not asked about that file again (a
refused file can still be approved with `--approve-plugin`). The question is only
ever asked at startup, never in the middle of a game.

**`--hidden`, `--headless`, `--no-dialogs`**: never asked. Plugins nobody decided on
are blocked, with the log line above.

**Command line** (each prints one JSON object on one line and exits; no window, no
disc, no log file):

    melee_source --approve-plugin <file.mufighter> [--settings-path <ini>]
    melee_source --revoke-plugin <sha256> [--settings-path <ini>]
    melee_source --list-plugins [--settings-path <ini>] [--fighters-dir <folder>|none] [--vanilla-only]

`--settings-path` decides which approvals file is used, exactly as for a run.
`--fighters-dir` (and `--vanilla-only`/`--vanilla-game`) decides which folder
`--list-plugins` lists, exactly as for a run. Exit codes: 0 done, 1 failed (file
unreadable, not a plugin, approvals file not writable), 2 bad command line (not
exactly one of the three, a missing value, a SHA-256 that is not 64 hex digits).

Every answer has `approvals_file` (the store used), `signing_keys` (how many public
keys this build trusts) and `test_key` (true only in a test build, below).

`--approve-plugin`:

```json
{"action":"approve","ok":true,"plugin":{"file":"wolf.mufighter","path":"C:\\...\\fighters\\wolf.mufighter","name":"Wolf","size":123456,"sha256":"<64 hex>","signature":"none","approval":"approved","approval_date":"2026-11-23T04:00:00Z","status":"approved"},"approvals_file":"C:\\...\\plugin-approvals.json","signing_keys":0,"test_key":false}
```

On failure: `{"action":"approve","ok":false,"error":"<path> is not a fighter plugin",...}`.

`--revoke-plugin` (records a refusal; `was` is what the store said before:
`approved`, `refused` or `none`):

```json
{"action":"revoke","ok":true,"sha256":"<64 hex>","was":"approved","approvals_file":"...","signing_keys":0,"test_key":false}
```

`--list-plugins` (`folder` is `null` for vanilla and `--fighters-dir none`;
`approvals` is the whole store, including files no longer present):

```json
{"action":"list","ok":true,"folder":"C:\\...\\fighters","plugins":[<plugin>,...],"approvals":[{"sha256":"<64 hex>","approved":true,"name":"Wolf","file":"wolf.mufighter","date":"..."}],"approvals_error":null,"approvals_file":"...","signing_keys":0,"test_key":false}
```

Each `<plugin>` is the object shown under `--approve-plugin`:
`signature` is `none`, `valid` or `invalid`; `approval` is `none`, `approved` or
`refused`; `status` is `signed`, `approved` or `blocked` (what a run would do with
it). A file that cannot be read has `error` instead of the hash fields.

**`--print-info`** has five more fields:

| Field | Meaning |
| --- | --- |
| `plugin_signing_keys` | public keys this build trusts (0 until the owner adds Nucleus's key) |
| `plugin_test_key` | true only for a build made with `-DMELEE_PLUGIN_TEST_KEY=ON`; never ship one |
| `plugin_approvals` | the approvals file this command line would use |
| `plugins_folder` | the folder plugins come from (`null`: none) |
| `plugins_blocked` | the plugins there that would be blocked: `{"file","name","sha256","signature","approval"}`, or `{"file","name","error"}` for one that cannot be read; `null` if the folder could not be examined |

## The approvals file

`plugin-approvals.json` in the folder of the settings file (`--settings-path`;
default `port-settings.ini` in the working folder, the same rule as the cosmetics
files):

```json
{
  "format": 1,
  "about": "...",
  "plugins": {
    "<sha256 of the plugin file, lowercase>": {
      "approved": true,
      "name": "Wolf",
      "file": "wolf.mufighter",
      "date": "2026-11-23T04:00:00Z"
    }
  }
}
```

The key is the SHA-256 of the whole plugin file; `name` and `file` are only for
people reading it. `approved: false` is a refusal (the player said no, or
`--revoke-plugin`). Writes take a lock file beside it (`plugin-approvals.json.lock`)
and replace the file whole, so two programs approving at once lose nothing and a
crash never leaves half a file. A file that cannot be read approves nothing (the
log and `approvals_error` in `--list-plugins` say why); the next approval or refusal
moves it aside to `plugin-approvals.json.unreadable` and starts a new one.

## Signatures

A signature is a separate file beside the plugin, named after it with `.sig` added:
`wolf.mufighter` -> `wolf.mufighter.sig`. (A separate file, not a field in
`fighter.json`, because a plugin folder holds plugins without a `fighter.json`, and a
signature that travels as its own file can be added by Nucleus without repackaging
the plugin.)

It is an Ed25519 signature (RFC 8032; checked with monocypher's
`crypto_ed25519_check`) of the 40 bytes

    "MUFTSIG1" + SHA-256(plugin file)

stored as 128 hex digits (a trailing newline and surrounding spaces are fine) or as
64 raw bytes. A signature that does not verify against any compiled-in key (wrong
key, changed plugin, damaged or empty `.sig`) is `invalid`: the plugin is then
treated exactly like an unsigned one, so it needs the player's approval, and the log
says the signature does not verify.

The public keys are in `port/app/plugin_keys.cpp`:

- `kProductionKey`: SSBM Nucleus's public key. **It is empty (all zero) on
  purpose**: the owner supplies the real key. An all-zero key verifies nothing, so
  until then every plugin needs the player's approval.
- the TEST-ONLY key, compiled in only when CMake is configured with
  `-DMELEE_PLUGIN_TEST_KEY=ON`. Its private half is in the repository
  (`port/tests/plugin_test_key/TEST_ONLY_ed25519_seed.txt`), so it proves nothing;
  such a build reports `"plugin_test_key": true` in `--print-info` and must never be
  released (the release script refuses it).

`tools/fighter_plugin_sig.py` makes and checks signatures in this format
(`sign --seed-file <key file> <plugins>`, `verify --public-key <hex|file> <plugins>`,
`public --seed-file <key file>`); Nucleus's own signing service must produce the
same bytes.

## Installs

Installing a fighter does not approve it. The mod browser keeps plugins it built or
downloaded in the installed-fighters folder as before; when one would be blocked its
note says it needs the player's approval, and the log names the file. The roster
installer installs fighter data only (no code). Both go through the same gate when
the game loads plugins.

## Tests

- `port_plugin_gate` (`port/tests/plugin_gate_test.cpp`): hashing, signatures (valid,
  tampered, malformed, wrong key, empty and all-zero keys), fighter names from
  synthetic and damaged plugin objects, the approvals file, the gate (blocked,
  approved, one flipped byte, signed, bad signature, refusal not asked again, copies
  held unchanged), the command line and the `--print-info` fields.
- `source_plugin_approval` (`port/tests/plugin_approval_test.py`): the same commands
  through the built program, with stdin closed and a 20 second limit.
