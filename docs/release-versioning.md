# Release versions

Release versions have three numeric components. After 0.9.99, continue with
0.9.100, 0.9.101 and so on until 1.0.0 is ready. Launchers compare these
components as integers, so 0.9.100 is newer than 0.9.99 and older than 1.0.0.

Avoid a fourth component or a suffix for public hotfixes. A launcher that only
compares the first three numbers may never offer such an update. Keep VERSION,
release tags, archive names and release notes on the same three-part version.

## Nucleus Unlocked starts its own line at 0.1.0

Nucleus Unlocked numbers its releases on a line of its own, starting at 0.1.0. It descends from
Melee Unlocked 1.0.5 and from this fork's 1.0.6 (release candidate 4), so 0.1.0 is the newer build
although its number is lower than every earlier one: never compare a Nucleus Unlocked version with a
Melee Unlocked or pre-0.1.0 number. Nothing in the client compares its own version (the "newer than"
check in `port/runtime/hle/slippi_version.h` is for Slippi's netplay version, `SLIPPI_SEMVER`); a
release feed or launcher that offers updates by number must start from 0.1.0 too, with no 1.0.x entry
left on it. From here the three-part rule above applies as before: 0.1.1, 0.1.2, ... 0.2.0.
