# Frame rate and rollback verification

Measured on an Apple M1 Pro running macOS 26.4.1, using x86-64 hosts under Rosetta and an NTSC 1.02 disc. The game library was built from the fork. The upstream version under comparison was 1.0.5.

| Claim | Observation |
| --- | --- |
| Game logic runs at 60 Hz | A paced, headless game ran 1,900 retraces in 31.668 seconds, averaging 16.667 ms per retrace. The scripted match produced the same RNG, fighter states and other digest fields in an unpaced run and a hidden graphics run requested at 144 FPS. All 76 fields agreed for all 572 overlapping match frames. |
| Intermediate frames are drawn | Eight synthetic 60 Hz source ticks produced 8, 20, 33 and 63 presentations at 60, 144, 240 and 480 FPS targets respectively. At 60 FPS there were zero re-posed draws. At the three higher settings each presentation had a re-posed draw. The diagnostic uses a hidden OpenGL window and a moving triangle with an authored animation track. Its counters measure draws submitted by the renderer. |
| Monitor rate, 480 FPS and uncapped gameplay | The full host accepts fixed, monitor and unlocked settings. Both Mac gameplay entry points disable the threaded presenter. The diagnostic exercises it independently. Physical display cadence and sustained 480 FPS gameplay were unmeasured. |
| Subframe input is on by default | A fresh configuration has early inputs off. The setting persists under `early_inputs2`. Starting rollback immediately on an early contradiction is a separate opt-in through `MELEE_EARLY_ROLLBACK=1`. |
| Inputs are read every millisecond and sent between frames | With early inputs enabled, the production sampling thread read a synthetic controller 1,705 times. Median interval was 1.277 ms and p95 was 1.431 ms. All 120 button changes reached the transport callback before the next simulated game poll, with 10.570 ms median lead. The sampler sleeps for 1 ms between reads, so scheduler overhead adds to the interval. This measures the host path through the callback. |
| Wrong predictions are corrected a frame sooner | In a controlled native prediction test, a late A press contradicted frame 100. With defaults, the game triggered rollback on tick 101 when the confirmed pad arrived. With `MELEE_EARLY_ROLLBACK=1`, it triggered on tick 100. Both rewound to frame 100. Real matches depend on arrival timing and peer support. |

In a separate loopback game with early inputs enabled and 8 ms of scripted lead, one client sent 154 early-pad packets. Its peer received all 154, used them for four predictions and confirmed all four. The other direction delivered 66 of 67 packets. Both recorded 969 frames, the same winner and zero desyncs. Neither client needed to start a rollback from an early packet during this game.

## What the latency numbers measure

The announcement slide gives 65.69 ms for controller to screen in Melee Unlocked and 67.71 ms in
Dolphin. The difference is 2.02 ms on the developer's setup. For key press to sound the slide gives
56 ms and 77 ms. Neither measurement timestamps the simulation frame that consumed a physical
press. A press close to a polling boundary can land on different ticks if the clients sample at
different times. Two clients can also consume the press on the same tick and display or play the
result at different times. An uncapped renderer alone does not change input assignment. These tests
did not measure physical latency or original GameCube hardware.

## Local Ranked sets

Two full macOS clients entered Ranked from the game's online menu using separate synthetic accounts and loopback ports. Both completed game 1, returned to setup for counterpicking, completed game 2 and recorded the same winner. The first run recorded 1,224 and 1,213 game frames. A second run recorded 1,344 and 1,229. One peer rolled back three times in its first game of the second run, with maximum depth one. All four games completed with zero reported desyncs. Both clients suppressed result submission for these local fixtures.

To reproduce with the packaged Mac build, run the harness with your own disc. It retains the logs and both fixture profiles in the output directory.

```sh
app=build-mac-full/MeleeUnlocked.app/Contents
python3 tools/check_local_ranked_set.py --fast \
  --exe "$app/MacOS/MeleeUnlocked" --sys-dir "$app/Resources/Sys" \
  --iso "/path/to/Melee.iso" --output /tmp/ranked-set-run
```

The optional `--fast` mode advances the game's network timeout counters faster than wall time. One early unpaced trial hit a peer stall and abandoned the set. Omit the option to run at the normal game clock.

## Official matchmaking and reporting

The fork implements the client side of the [Slippi matchmaking protocol](https://github.com/project-slippi/slippi-rust-extensions/blob/934f373d3cbcbca9ea3ceb68b53bda875c96abfb/matchmaking/src/protocol.rs).
Slippi runs the production queue and leaderboard. On November 22, 2026 the Mac transport completed
TLS and validated the HTTP 101 WebSocket upgrade at `wss://matchmaking.slippi.gg/v1/queue`.
It sent a close frame before any account or matchmaking request.

The [queue test](../tools/check_slippi_ranked_queue.py) runs the fork and Dolphin's shipped
matchmaking library against a local server, verifies their Ranked requests and assigns opposite
seats. The [peer test](../port/tests/slippi_ranked_peer_interop.py) exchanges Ranked setup and recovery
packets in both directions over ENet. The [reporting test](../port/tests/slippi_ranked_report_test.py)
uses a local HTTP fixture to check game reports, compressed uploads, set completion and rating-result
parsing. Official queue admission, result acceptance and a real rating change require a live set.

## Stock Dolphin interoperability

A signed, unmodified Slippi Dolphin netplay 3.7.0 client and the packaged fork each selected Ranked from the online menu, loaded local fixture profiles, and requested mode 0. A loopback WebSocket fixture assigned two UDP endpoints. The games negotiated their own peer connection, exchanged stage selections and controller input, and wrote independent recordings. The macOS network sandbox blocked external connections for both processes, including production result submission.

**The final build completed a 2–0 Ranked set against stock Dolphin.** Both clients reported 1,498 frames on Final Destination and 1,256 frames on Dream Land. They agreed on the winner, stage and normal game-end event in both games. All 5,472 finalized per-fighter records across their four recordings matched byte for byte. The fork reported zero desyncs in both games. Stock Dolphin sent the reverse-order stage strikes 28 and 8 during game 1, and the fork accepted them. An earlier build had rejected that valid stock input.

In an earlier trial, both completed a 1,298-frame game on Battlefield. The fork rolled back once by one frame. Of 2,578 per-fighter records, one differed in Bowser's offscreen flag at frame 783. During game 2 on Yoshi's Story, stock Dolphin's Ranked quality guard stopped the match at frame 420, reporting a performance ratio of 2.29 and average ping of 1.0 ms. The clients recorded different final frames following that disconnect. One of 840 overlapping per-fighter records differed in the same offscreen flag. Coordinates, action states, damage and stocks matched for all overlapping records in both games. The stock specification maps this flag to mask 0x80 of post-frame byte 0x2A. [Slippi post-frame specification](https://github.com/project-slippi/slippi-wiki/blob/3a85ce8b1f2dae1b487b2529a43c80f5de5885a3/SPEC.md#state-bit-flags-5)

The successful set used extra A and Start presses through Dolphin's test controller on its character
select screen. The harness now issues those presses automatically. The menu automation timing was
adjusted after that run.

Run the stock-client check on macOS with Python 3.11+, the `melee` and `websockets>=14` Python packages (verified with 0.47.6 and 17.4 respectively), and the signed stock netplay app. The harness gives Dolphin a named-pipe controller and enables libmelee's menu observation code only in a temporary profile. It prints both clients' game reports and compares every byte of their finalized per-fighter records.

```sh
python3 tools/check_stock_ranked_set.py \
  --dolphin "/path/to/Slippi Dolphin.app" --fork build-mac-full/MeleeUnlocked.app \
  --iso "/path/to/Melee.iso" --output /tmp/stock-ranked-set
```

## Reproduce

Build the sampler from the repository root on an x86-64 host. On Apple Silicon, add `-arch x86_64` to the compiler invocation.

```sh
c++ -std=c++17 -O2 -pthread -Iport/runtime/host -Iport/runtime/hle -Iport/runtime/ppc \
  tools/probe_early_sampler.cpp port/runtime/host/early_input.cpp -o /tmp/probe_early_sampler
/tmp/probe_early_sampler
```

The probe alternates two buttons six milliseconds after each 16.667 ms poll. It records the sampler's actual read times and when its callback receives each target frame and pad.

The native correction fixture in `tools/probe_early_rollback.c` uses the game's own prediction buffers and functions. Compile it with the same definitions and include paths as the native game's `shim/mu_online.c`, adding `-ffunction-sections -fdata-sections -Wl,--gc-sections`. Run it once with default environment and once with `MELEE_EARLY_ROLLBACK=1`.

Run the Mac offline host with `MELEE_MAC_DEV_SPIKE=1` and `--presenter-self-test --fps 144` for the GPU diagnostic. Repeat at 60, 240 and 480. For simulation parity, run a separate instance for each mode against the same local disc and game library. Use fresh card directories, `--vanilla-game --no-gc-adapter --time-base 0 --rng-seed 0x12345678 --script port/scripts/parity_vs.txt --frames 1900`, and supply a different `--state-digest` path for each run. The three modes were paced `--headless`, unpaced `--headless --fast`, and `--hidden --fps 144`. Compare the CSVs with `python3 port/mac/compare_digest.py <reference.csv> <observed.csv>`.

The relevant paths are [the presenter fixture](../port/linux/renderer_presenter.cpp), [the full Mac gameplay renderer selection](../port/app/main.cpp), [the sampler](../port/runtime/host/early_input.cpp), [the settings loader](../port/runtime/gx/pc_settings.cpp), and [the native prediction logic](../sourceport/game/shim/mu_online_prediction.inc).
