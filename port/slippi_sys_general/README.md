# Slippi Sys files

Copied from the Slippi Ishiiruka repository (GPL-2.0): the Gecko code list
(`GameSettings/GALE01r2.ini`, which the build reads: `tools/native_recording_codes.py`), the code handler and bootloader the port installs at boot, and the
`GameFiles` diffs the EXI device serves to the game (menus, HUD, Slippi UI). The port reads this
folder at run time (`--sys-dir`, default `port/slippi_sys` relative to the working directory).

Slippi's `GameSetup_gui.dat`, `slpCSS.dat` and `SlippiCSS_gui.dat` are not here: they are mostly
Nintendo's own disc assets, so the game rebuilds them from the player's disc with the deltas in
`port/sys_deltas` (`port/runtime/host/sysgen.cpp`; a release carries them in `deltas/`).
