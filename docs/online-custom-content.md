# Custom fighters and stages online

Status: fighters built (branch `opp-content`); stages designed here, not built.

## The rule

A custom fighter (and, second, a custom stage) is selectable online only when **every player in the
session already has the byte-identical package installed**. Nothing is ever downloaded during match
setup and nothing is fetched from a peer or from SSBM Nucleus. Otherwise the fighter is shown greyed
out on the character select for the player who has it, with a plain line saying why, and A on it plays
the error sound. Also:

1. every machine is Nucleus and offers the capability bit for that kind of content;
2. the mode is Direct or Teams (never Ranked or Unranked: those match strangers on the retail game).

Anything else plays exactly as today.

## Capability (both halves)

The build message 0xE0 is append-only. After the product word and its version comes a fourth
capability byte `caps4` ("Nucleus content"), one byte shared with the items and Squad Strike jobs:

| bit | meaning |
| --- | --- |
| 0x01 | items rule (`MU_BUILD_CAPS4_ITEMS`, items job) |
| 0x02 | custom fighters (`MU_BUILD_CAPS4_CUSTOM_FIGHTERS`) |
| 0x04 | custom stages (`MU_BUILD_CAPS4_CUSTOM_STAGES`, reserved) |
| 0x08 | Squad Strike in Direct (`MU_BUILD_CAPS4_SQUAD`, squad job) |

A peer takes custom fighters only if its 0xE0 arrived, named the product `nucleus`, and set 0x02.
Detection, per remote: no 0xE0 within 3 s of the connection is Slippi Dolphin or a pre-0xE0 build; an
0xE0 without the product word is Melee Unlocked; one without `caps4` or the bit is an older Nucleus.
In all three cases every added fighter is greyed out ("P2's game cannot use added fighters.").
`MELEE_TEST_NO_CUSTOM_FIGHTERS=1` clears the bit and this machine's own offer, sends no list and
ignores lists, to play an older Nucleus in tests. `MELEE_TEST_ROSTER_GREY=<mask>` greys those roster
fighters on the offline select (tests only: agent copies cannot log in to reach Direct's select).

## Fighters

**Identity: the content hash.** SHA-256 over `nucleus-fighter-2`, the roster record the game is
handed (fighter.json's name, file names, symbols, costumes, bones, action table), then each file the
roster serves for the fighter, in lower-case name order, as name and SHA-256 of its bytes. Left out:
the files a match never opens, so two players with the same fighter but other sounds are not kept
apart: sound banks and music (`.ssm`, `.sem`, `.hps`, and the fighter's `sound\` folder, which is not a
served file), movies (`.thp`, `.mth`), and the intro, ending and results scenes (`ftDemo*`, `GmRst*`).
Kept in: the fighter data, animations, costume files (the match reads the skeleton from them, so a
costume file is not presentation only) and effect files (their code runs inside rollback). Folder
name, install time and `nucleus.json` are not hashed. The local id (26 + position in the roster,
which follows install order) is never trusted across machines.

**Offered online:** every installed fighter in the first 16 that brings no code of its own (a
`.mufighter` plugin waits for plugin trust) and serves no file name another installed fighter already
serves (the first owner of a name wins on each machine, so a clash could load different bytes on each
side). Whether it came from SSBM Nucleus does not matter: the content hash decides.

**Announce.** Message 0xEC `NP_MSG_MU_CONTENT`: `'M' 'U' 'C'`, version 1, kind 1 HAVE = sender port,
count (0..16), then per fighter {the sender's id for it (26..41, increasing), content SHA-256}: 7 + 33
bytes per fighter, at most 535. Sent to each peer with the bit once its 0xE0 arrived, and again to all
of them whenever this machine's offered set changes (`NetplayClient::SendFighterListIfDue`, every
frame the online select polls). Decoded strictly (`hle/mu_content_wire.h`) and bound to the ENet
sender's negotiated port; a later list replaces an earlier one. Kind 2 is kept for stages.

**Verdict,** per fighter of this machine, every frame (`host::online_fighters`): before any peer is
connected, every offered fighter is selectable (a Direct or Teams search may start with one); once
connected, it is selectable only if every peer took the bit and every peer's list has its content
hash. While a build message (3 s) or a list (10 s) is on its way the verdict waits; after that a peer
without one counts as a game that cannot use added fighters. Each machine computes the verdict on its
own fighters; a fighter only one side has is greyed on that side, and the other side has no icon for
it at all (it has no such fighter). Both sides agree because the lists are symmetric.

**Map.** A remote 0x82 that locks in an id of 26 or more is mapped through that peer's list to the
content hash, then to this machine's fighter with that hash; the match block holds local ids. The
pick never goes out until every player has the fighter, so the mapping is always there by then (lists
go before the 0x82 on the same ordered channel); a lock-in that does not map is refused as a foreign
character, as before. Whether the pick may go out is judged on what arrived (every remote's build bit
and list, `online_fighters::every_peer_has`), not on the select screen's verdict: a pick locked
before connecting is sent the moment the connection is made, before the screen's next look.

**Setup hash.** `setup_inputs_hash` is unchanged: every side hashes the ids as sent. In
`setup_block_hash`, when a port's fighter is custom its id byte becomes 0xFE and its content hash is
folded in after `nucleus-fighters-1`, so sides with different local ids agree, and a disagreement on
which fighter it is can never start. With no custom pick the bytes hashed are exactly today's.

**Refusal.** A pick made before connecting that turns out not to be on every machine is unlocked once
the verdict is known, with the line saying why, so the player picks again; its 0x82 went out as "no
character locked" all along. Pick previews of an added fighter go out as "no fighter".

**Replays.** Game Start records the local id, meaningless elsewhere, and a replay plays without the
installed fighters (the host serves them only outside replays): id 26 is Master Hand there, and the
game's own check (`mu_match_check`) lets that through, so before this a replay of an added fighter
played Master Hand. Built: an online recording's metadata gets `nucleusFighters`, an object of port
(0-based) -> `"<content SHA-256 hex> <name>"` (`NativeReplayStream::set_nucleus_fighters`, set from
`online_fighters::match_fighters()` at Game Start); playback of a file with that key is refused the
way the game refuses a replay it cannot play: one log line ("... was played with an added fighter
(Slippy), and replays with added fighters cannot be played yet; the match is not started"), exit 5
(`kReplayRefusedExit`), no recording written. Not built: playing them (map the hash to an installed
fighter, serve the roster in replays, rewrite the Game Start ids).

## Stages (designed, not built)

Same shape. Identity: a content hash over the base stage id (the retail stage whose code runs it), the
served stage file bytes and any metadata that changes play (spawn points, blast zones); music, and
any other file the match does not read, left out. Announce: kind 2 HAVE_STAGES of 0xEC, the sender's
installed stages by content hash with its own page-2 id. Page 2 of the stage select greys out each
stage not every player has, with the same kind of line, and only when every peer set 0x04. Random never
lands on a greyed stage. Setup hash: the stage id becomes a marker and the content hash is folded in.

## Stock Slippi and older Nucleus peers

Detected as above. Added fighters are then greyed out, and no new byte reaches such a peer: 0xEC goes
only to peers with the bit; the 0x82 of an added fighter never goes out (it is greyed, and a pick made
before connecting is sent masked, "no character locked", then unlocked); pick previews of an added
fighter go out as "no fighter". The only new byte on the wire toward any peer is `caps4` at the end of
0xE0, which older builds stop reading before.

## What the player sees

Melee's own online select screen and its own text lines, no overlay. Added fighters have their icons
on the bottom row's corners in Direct and Teams. One not every player has is dark grey; with the hand
over it the free status line says "P2 does not have Slippy installed." or "P2's game cannot use added
fighters." (Teams: the first player who lacks it); A plays the error sound. A pick unlocked after
connecting keeps that line until another fighter is locked in. Right after connecting, for a moment:
"Checking P2's fighters."

## Risks

- **Metadata-only differences.** Title, description or folder name changes do not matter (not hashed);
  a repackaged zip with identical files has the same content hash.
- **Sound banks and other presentation files.** Not hashed (see Identity): two players may hear
  different sounds for the same fighter. A presentation file that the simulation does read after all
  would be a desync risk; the list is short and named.
- **Clone determinism under rollback.** Proven by loopback runs with `--lag-ms` forcing rollbacks.
- **Hashing time.** The offered fighters are hashed on the first connected frame (about 50 ms per
  10 MB), cached by file size and time for the run.
- **Hostile peer.** A peer only names ids and hashes; nothing is downloaded, and a lock-in that does
  not map to a fighter this machine has is refused.
