# tools/

One line per script. Run the Python ones with `C:\Python313\python.exe` from the worktree root
(`gdb_*.py` run inside gdb, `*.sh` under Linux or WSL). One-off and dead
scripts were removed on 2026-11-24 (structure audit item 8); they are in the history of
`netlee-main` at 7e3e436a. "Kept, see note" marks a script that is unused today but was kept for the
reason given under [Notes](#notes).

## Worktree, build steps, tests

| Script | What it does |
| --- | --- |
| `new_worktree.py` | a ready-to-build worktree with its own test install and port block; `--deploy`, `--list`, `--remove` |
| `decomp_fork.py` | the private decomp fork: `setup`, `status`, `verify`, `push`, `export-patch`, `public-snapshot`, `import-branch` (`--base rebased`: against upstream 8e6529547) |
| `decomp_layers.py` | the fork's squash rebuilt as layers on upstream 8e6529547 (`build`), other branches moved onto them (`replay`); docs/decomp-fork-history.md |
| `prepare_native_sources.py` | game configure step: checks the decomp checkout (applies the patch only in a public snapshot) |
| `refresh_native_patch.py` | old habit kept working: regenerates the untracked local `melee-native.patch` from the fork |
| `run_tests.py` | runs the registered ctest suites: `quick`, `game`, `slow`, `all` |
| `generate_fobj_host.py` | host build step: the host-only FObj animation adapter |
| `native_recording_codes.py` | host build step: replay metadata for the native General Codes |
| `fetch_graphics_dlls.py` | host build and release step: fetches the pinned DXC and librashader libraries into `port/third_party` |
| `disc_offset_audit.py` | game build check: every `DISC_STRUCT` field offset against the decomp's annotation |
| `native_major_fixture.py` | game test input: extracts the major-mode runner for the isolated boundary fixture |
| `snapshot_audio_ranges.py` | game build step: rollback snapshot exclusions for the audio driver, from the linker map |
| `check_collision_geometry.py` | game test: collision display lists against their vertex arrays |
| `skin_corpus.py` | host test helper: runs `port_skin_corpus` over a folder of skin zips |
| `test_slp_diff.py` | host test of `slp_diff.py` |

## Fighter plugins and Training Mode CE

| Script | What it does |
| --- | --- |
| `mexft/mexft_prepare.py` | game build step: prepares a fighter's MexTK C for the native game |
| `mexft/mexft_overlay.py` | game build step: the MexTK headers fighter modules build against |
| `mexft/mexft_abi.py` | game build step: the plugin ABI id header |
| `mexft/mexft_syms.py` | game build step: `melee_game.syms`, the symbols plugins link against |
| `mexft/mexft_plugin.py` | game build step: links a fighter into a `.mufighter` plugin |
| `mexft/mexft_kit.py` | writes a fighter plugin kit (what the Nucleus mod tool compiles with) |
| `mexft/mexft_build.py` | the kit's driver: builds one plugin from a fighter's C |
| `mexft/mexft_abi_audit.py` | finds MexTK functions whose parameters disagree with the decomp |
| `mexft/mxdt.py` | lists/exports an m-ex disc's fighters; writes `mu_mex_retail.inc` |
| `fighter_plugin_sig.py` | Ed25519 detached signatures of `.mufighter` files, as the plugin gate checks them |
| `fighter_donor_tables.py` | the retail move tables' rows and hashes `host/fighter_package.cpp` compares a classic fighter.yml's against (from `main.dol` + `symbols.txt`) |
| `tmce/tmce_upstream.py` | TM-CE's pinned upstream checkout for the tools here |
| `tmce/prepare_src.py` | game build step: TM-CE's C made from the pinned upstream commit |
| `tmce/native_edits.py` | the hand edits TM-CE's C needs to compile natively (run by `prepare_src.py`) |
| `tmce/gen_mextk.py` | game build step: native MexTK headers and the symbol map for TM-CE |
| `tmce/gdb_decomp_info.py` | game build step, run inside gdb: types, functions and globals of `melee_game.dbg` |
| `tmce/link_module.py` | game build step: links one TM-CE module into a single object |
| `tmce/mextk_layout.py` | library: console layout of the MexTK headers |
| `tmce/mextk_symbols.py` | library: MexTK names to decomp symbols by console address |
| `tmce/check_signatures.py` | checks TM-CE's MexTK prototypes against the decomp's |
| `tmce/asm_catalog.py` | lists every TM-CE ASM patch and the decomp function it lands in |
| `tmce/twin_debug.py` | shows MexTK members and decomp rows around a console offset |
| `tmce/dat_symbols.py` | lists the public symbols of HSD `.dat` archives (kept, see note) |
| `tmce/dol_disasm.py` | disassembles retail `main.dol` around an address with decomp names (kept, see note) |

## Online, netplay, replays

| Script | What it does |
| --- | --- |
| `p2p_pair.py` | two hidden instances play one peer-to-peer rollback match; PASS needs 0 mismatched |
| `online_quad.py` | 2-4 hidden instances play one match through the online menus on local peering |
| `online_pair.py` | two instances through an online match, reports their time sync |
| `net_fault_proxy.py` | UDP proxy that adds repeatable lag and loss between two local games (`p2p_pair.py --lag-ms --loss`) |
| `digest_compare.py` | first frame where 2 or 4 `--state-digest` CSVs disagree |
| `online_motor_check.py` | checks the online rumble trace in a `p2p_pair.py` output folder |
| `slp_diff.py` | first difference between two `.slp` recordings of one online game |
| `replay_compare.py` | replays a `.slp` on the Source Port and checks every reference player-frame |
| `replay_batch.py` | runs `replay_compare.py` over many recordings (docs/replay-validation.md) |
| `fighter_digest.py` | strict comparison of declared Fighter fields at Slippi post-frame points |
| `fieldmap/field_diff.py` | first differing Fighter fields between a console-layout and a native dump |
| `fieldmap/bone_diff.py`, `fieldmap/float_first.py` | first differing bones / floats between the same two dumps (kept, see note) |
| `fieldmap/digest_context.py` | Fighter fields around a digest's first difference (kept, see note) |
| `fieldmap/slp_items.py` | item events of a replay for a frame range (kept, see note) |
| `fieldmap/gdb_fighter_map.py`, `gdb_struct_map.py`, `gdb_layout.py` | run inside gdb: console vs native layouts of structs |
| `fieldmap/rebind_schema.py` | binds a digest schema to another game library after checking it fits |
| `slippi_trace_diff.py` | compares two `MELEE_TRACE_SLIPPI_CMDS=1` logs |
| `lockstep_compare.py` | compares per-frame state CSVs (named by `port/scripts/parity_vs.txt`) |
| `check_slippi_ranked_queue.py` | Ranked matchmaking against a Slippi Dolphin 3.7.0 bundle on loopback |
| `check_stock_ranked_set.py`, `check_local_ranked_set.py` | a Ranked set against stock Slippi Dolphin / between two local clients |

## Mods, cosmetics, Gecko codes

| Script | What it does |
| --- | --- |
| `gecko.py` | library: reads Gecko code INI files and builds and parses code tables (`.gct`); used by `native_recording_codes.py`, `gecko_targets.py` and tests |
| `gecko_targets.py` | which console data a Gecko code may write on the Source Port (writes committed tables) |
| `gecko_functions.py` | which functions a Gecko code may make return at once |
| `gecko_coverage.py` | how much of the Gecko codes in the tree the Source Port can run |
| `cosmetic_import_diagnostic.py` | read-only check of a cosmetic mod bundle |
| `companion_texture_diagnostic.py` | maps CSS portrait and stock texture payloads |
| `effect_cosmetic_diagnostic.py` | validates effect DATs against the clean disc |
| `effect_slot_survey.py` | read-only: who spawns each model effect and particle generator of a fighter's `Ef*Data.dat` (the survey behind `port/runtime/host/effect_slots.cpp`'s slot table) |
| `stage_cosmetic_diagnostic.py` | proves a stage DAT differs from retail only in image bytes |
| `nucleus_vault_diagnostic.py` | read-only check of a saved Nucleus cosmetic project |
| `retail_costume_hashes.py` | sizes and SHA-256 of the 124 retail costume files (opponent skins) |
| `skin_fetch_standin.py` | local stand-in for the Nucleus post, download and export routes of opponents' skins (prints whether each was a `view=1` view or a counted download; `--view-400`: the download route refuses `view=1`, as the live site does today) |
| `mods_menu_real_smoke.py` | read-only smoke run of the Mods menu against the real site |
| `import_gd_kit.py` | imports GD's font assets and glyph metrics |

## Release and packaging

| Script | What it does |
| --- | --- |
| `package_release.py` | Windows release packager (used by `windows-build/client_release.py`) |
| `windows_signing.py` | optional Authenticode signing of the release programs |
| `build_recipes.py` | collects warmed pipeline recipes from scripted runs (kept, see note) |
| `package_linux_release.py`, `build_linux.py`, `build_linux_release.sh` | the Linux build and release |
| `package_native_linux.py` | private Linux test package (docs/linux-native-0.9.md) |
| `package_source_port.py`, `export_source_port_noslippi.py` | folder / source export of the build without Slippi (kept, see note) |
| `crash_triage.py`, `crash_report_privacy.py`, `crash_report_text.py` | group, scrub and print crash reports |
| `pull_reports.py` | downloads reports from upstream's old crash relay and its Discord report channels (kept, see note) |
| `extract_dol.py` | extracts `main.dol` from a disc |
| `melee_iso.py` | library: finds the retail disc without a path in the repository |
| `profile_symbolize.py` | names the raw `--profile` samples of `melee_source.exe` |

## Decomp porting layer (kept for audit item 10)

The rewriters and censuses that made the mechanical part of the decomp fork. `decomp_layers.py build`
reruns guard_asm, gxpipe_rewrite, disc_convert, disc_wrap and the two `MU_P` wrappers to make fork
branch `netlee-rebased` (structure audit item 10; docs/decomp-fork-history.md); two are also build steps.
`tag_no_contract.py` and the censuses that read `port/generated_vanilla` (`fma_census.py`,
`fma_exact/fma_site_census.py`, `count_compare.py`, `retail_ops.py`) have no input any more: that folder
was the Static Recomp's translation of the retail code, removed with it on 2026-11-24.

| Script | What it does |
| --- | --- |
| `cast_census.py` | lists (`--fix`: fixes) integer-to-pointer casts narrower than a pointer |
| `disc_types.py` | the struct types that map disc data (also read by `tmce/gen_mextk.py`) |
| `disc_convert.py` | marks named structs as disc layout (`DISC_STRUCT`, `DISC_PTR` slots) |
| `disc_wrap.py` | wraps reads of disc pointer slots in `DP()` |
| `disc_unwrap.py` | undoes the `DP()` wraps the compiler rejects |
| `disc_audit.py` | finds `DISC_STRUCT` fields that compile but have the wrong layout |
| `guard_asm.py` | wraps PowerPC assembly functions in `#ifndef MU_NATIVE` |
| `gxpipe_rewrite.py` | rewrites GX write-gather pipe stores as calls |
| `gcc_census.py` | syntax-checks every game file with GCC and summarizes the errors |
| `fma_census.py` | where the console compiler fused multiply-adds and where GCC does |
| `fma_exact/gcc_fma_wrap.py` | compiler launcher that makes fused multiply-adds round like the GameCube |
| `fma_exact/tag_no_contract.py` | tags functions the console never fused |
| `fma_exact/wrap_sqrt_args.py` | places the square-root argument wrappers |
| `fma_exact/wrap_square_products.py` | wraps squares of repeated expressions in `MU_P()` |
| `fma_exact/fma_site_census.py` | per-function fused-op census, retail vs native |
| `fma_exact/count_compare.py` | per-function fused-op counts of two site lists |
| `fma_exact/retail_ops.py` | the console's float operations for one function |
| `fma_exact/native_ops.py` | the native float operations for one function |

## The tools' own tests

`test_crash_report_text.py`, `test_digest_compare.py`, `test_fighter_digest.py`, `test_jev_review.py`
(with `jev_review.py` and `jev_review.ps1`), `test_replay_validation.py`, `test_windows_signing.py`:
registered as `tools_<name>` in the top-level `CMakeLists.txt`, label quick.

## Other files here

`vs_stages.txt` (the stage ids a VS sweep may use), `fieldmap/*.json`, `mexft/*.txt` and
`mexft/m-ex/` (an m-ex header copy; the plugin layout uses `sourceport/game/tmce/MexTK`),
`tmce/*.json`, `tmce/fakeinc/`, `tmce/native_fixes.patch`, `latency_probe/` (standalone input and
audio latency probes), `probe_early_*.c*` (docs/announcement-verification.md).

## Notes

- **TM-CE and fieldmap debugging aids** (`dat_symbols`, `dol_disasm`, `bone_diff`, `float_first`,
  `digest_context`, `slp_items`): unused by any workflow, but part of live kits.
- **`build_recipes.py`:** `package_release.py` ships `shadercache/recipes.bin`; this makes such a file.
- **No-Slippi build:** `export_source_port_noslippi.py` was updated with the fork workflow;
  `package_source_port.py` is what `test_windows_signing.py` tests.
- **Upstream accounts** (`pull_reports.py`, `crash_triage.py`, `windows_signing.py`): left to audit
  item 5. `post_discord.py` (release posts to upstream's
  Discord channel) and its test are removed.

## Elsewhere

- `windows-build/client_release.py` (+ `client_release_test.py`): the Source Port client release
  ([docs/client-release.md](../docs/client-release.md)).
- `sourceport/tools/`: game build steps `check_elf_load.py`, `check_game_image.py`,
  `console_long_overlay.py`, `native_cpu_overlay.py`; their registered tests `test_audio_ranges.py`,
  `test_check_game_image.py`, `test_console_long_overlay.py`; and `cc1.sh` (compiles one game file
  into a scratch object, without touching the build).
- `port/scripts/`: scripted-input files for `--script`.
