# Shared Adventure: shared story scenes

Status (28 October 2026): implemented and covered by unit and relay tests; **not yet run in a live game**. The live
checks are under "Shared Adventure" in `tests/coop-final-checklist.md`. Eevee 1.0.0 only: the Pikachu build has no
addresses for the flag and Bag calls yet.

Roadmap stage 4. An opt-in mode, **off by default**: Social stays the default, and in Social mode nothing in this
document happens. The user guide is in [COOP-ONLINE.md](COOP-ONLINE.md#shared-adventure).

## What is shared

One mapped, reversible scene, chosen to start small rather than share arbitrary event flags or copy saves:

| | |
| --- | --- |
| Scene | Route 1: the Poké Mart clerk's free sample (sub event 1, AMX script 470 in `script_amx.gfpak`) |
| Completion flag | `FE_SUB_01_CLEAR`, hash `0xeb67b6c1fc530667`, save event flag **2179** (event table index 3 + 0x880) |
| Story point (prerequisite) | `FSYS_ARRIVE_ROAD001` (Route 1 visited), hash `0x696a6f94ba19dd02`, save flag **154** (system index 26 + 0x80) |
| One-time gift | item 17, **Potion** x1 |
| Work values | none |

The script: `FlagGet(FE_SUB_01_CLEAR)`; when unset, the clerk's text (`msg_sub001_clerkm_01/02` in `road001`), then
`ItemAddCheck(17, 1)`, `ItemAdd(17, 1)` and `FlagSet(FE_SUB_01_CLEAR)`; when set, his after-text only. So a partner who
gets the scene shared has exactly the state the script would have left: one Potion and the flag, and the clerk won't
give a second one.

It is reversible: clearing flag 2179 and taking one Potion restores the old state.

Why not Oak's parcel: the parcel, the Pokédex and the rival scene advance the main story state machine
(`WK_SCENE_MAIN_MASTER` and other scene work values) and move a key item. Setting those on another save is not
"selected flags and one gift"; it would need its own mapping of every scene work value and is left for later.

### How the flags were found (static analysis, `tests/logs/shared-adventure/`)

- `bin/flagwork/*.tbl` are `AHTB` tables of `{u64 FNV-1a 64 hash, u16 length, name}`; `ahtb.py` dumps them
  (`flag-names.txt`). Field scripts pass these hashes to `FlagGet`/`FlagSet`.
- The game's flag index lookup `0x70001c4030` (decompiled to `tests/cache/ghidra/out/sa-flags2.c`) searches eight
  tables in a fixed order with bases 0, 0x80, 0x180, 0x280, 0x880, 0xa80, 0xe80 and 0xf00. The strings in `main` give
  the order: zone, system, vanish, `VanishFlagAutoTable`, event, `trainer_id_hash_table`, time, real-time. Checks: the
  trainer flag 2742 found earlier is 0xa80 + 54 (`TRID_INSECT_02`); the early save `tests/saves/original` has flags 134-139
  = system 6-11 (`FSYS_XMENU_OPEN_*`) and 179/180 = `FSYS_ARRIVE_TOWN001/002`.
- All 705 scripts were disassembled with the trainer work's `amxdis.py` (`dis/*.dis`) and annotated with flag names
  (`annotate.py`, `dis/*.ann`). Script 470's message labels match `road001`'s message table (`msg-labels.txt`).
- `party-fixtures flags <save>` (new) prints every set flag and non-zero work value of a save.

### Game calls (`runtime/hle/coop_scene_apply.c`)

| Address | Use |
| --- | --- |
| `0x70001c3fb0` | FlagGet core (event work, hash), as the `FlagGet` native `0x70001a6870` calls it |
| `0x70001c41e0` | FlagSet core, as the `FlagSet` native `0x70001a67b0` calls it, under the event work's lock (`work+0x48`) |
| `0x70001c8e20` | Bag add (bag, item, count, new), the call the script's `ItemAdd` uses (`runtime/hle/gift.c`) |
| `0x70001cddf0` | the save image build (hook chained with the gift and the trade) |

Event work = world`+0x98`(field manager)`+0x58`(save data)`+0x80`; Bag = save data`+0x68`.

## Rules

- **Opt-in.** "Adventure mode: Social / Shared" in `coop.ini` (`adventure=social|shared`, default social), shown in
  the launcher's online settings and in Friends. Shared also needs **this session's consent** ("Share story scenes this
  session" in Friends), cleared for every new room. The relay offers a scene only between two players who both have
  Shared on and consent (mode 2 below). The HUD always shows the mode.
- **Social mode never applies the ledger.** An offer is answered "not sharing" and nothing is recorded; an accepted
  entry left from Shared mode waits; Catch up is refused.
- **Only the listed flags and the one gift change**, and only on the receiving player's own save, after its own
  answer. The wire carries a scene id only: a peer can't name a flag or an item. A scene id the build doesn't map is
  ignored.
- **Personal progress is never in the ledger.** `coop_scene_valid` refuses any entry with a trainer flag (`TRID_*`) or
  a flag naming a gym, badge, rival, champion, League, game clear or a `_WIN`/Red battle, and `coop_scene_check`
  returns "personal" for it.
- **The story point.** The partner's save must have the prerequisite flags and not the completion flag. Not reached:
  Ready is disabled and the scene waits for Catch up. Already completed: answered "already", no prompt.
- **Watching together** is not implemented: this is "shared completion" only. The scene's own script runs only on
  the initiator's game; the partner gets its result. Both avatars stay where they are (no "stay together or return"
  choice is needed).

## Flow

1. **Completion.** The game thread polls each mapped scene's flags four times a second. The first poll after the save
   loads is the baseline; a later change from unset to set is a completion on this save.
2. **Offer.** If both players share (mode 2), the initiator's client offers it at once (Y1). Otherwise it shows "Scene
   complete: share it with your friend from Friends", where **Share with your friend** offers it later (also for
   scenes completed before the session). Participation: "Offer my scenes to Everyone / Nearby" (`scene_offer=` in
   `coop.ini`); Nearby means tagged up, or both on the overworld within 30 m. Ask is the only mode: nobody joins
   without the prompt.
3. **Ready prompt** on the partner (60 s): **Ready** / **Not Yet** / **Stay Out**, with what Ready gives. B or Esc is
   Not Yet. The initiator sees "Waiting for …" with **Cancel** and **Continue Without** (also in Friends).
4. **Answers.**
   - Ready: the ledger says `accepted`; the scene is applied at the next overworld update outside a script or menu.
   - Not Yet: `pending`; Friends lists it under Story scenes with **Catch up**.
   - Stay Out: `declined`; never applied, later offers are answered "stay out" without a prompt.
   - Cancel (initiator, before the answer): the prompt closes and nothing is kept (a scene that was already pending
     stays pending).
   - Continue Without (initiator): the prompt closes, the scene stays pending for Catch up.
   - No answer in 60 s, or the initiator gone for more than 30 s: the prompt closes, pending.
   - The partner's answer is final once given: a cancel that crosses it on the wire changes nothing.
5. **Catch up** is explicit (a button per pending scene) and works offline, in Shared mode only. It needs the story
   point. Catching up a scene that is accepted, done or declined does nothing.

## The ledger and exactly-once

`<save>.scene` next to the save (like `<save>.trade`): `LGSCENE1`, then `scene <id> pending|accepted|done|declined`.
Written to `.tmp`, fsynced and renamed over the old one with `plat_replace_file`. It records consent, not the gift.

The gift can't be given twice because the **completion flag in the game is the idempotency key**:

- The apply checks the story point and the flag first (`coop_scene_check`), adds the item, and only if that succeeded
  sets the flag. Both happen in one game-thread step, and the save image is built on the game thread, so every save
  image has both or neither.
- An entry is applied only while it is `accepted`, the game's flag is unset and it wasn't applied in this run.
- It becomes `done` after a whole-save write whose image was built after the apply (the save-image hook, then
  `coop_scene_after_replace`), or at boot when the loaded save already has the flag.

| Interruption | What happens |
| --- | --- |
| During the prompt | `pending` on disk; Catch up later. |
| After Ready, before the apply | `accepted`; applied at the next overworld update after the restart. |
| After the apply, before a save | The running game had it, the save on disk doesn't: applied again after the restart, once. |
| After the save, before `done` was written | The loaded save has the flag: `done` at the first poll, nothing applied. |
| A failed save write | Still `accepted`; the flag is set in the running game, so nothing is applied again; the next save makes it `done`. |
| A save image built before the apply | Doesn't count: `done` needs an image built after it. |
| Bag full (the Potion stack at 999 or the pocket full) | Nothing is given or set; retried every 10 s with a notice. |
| The player does the scene natively while `accepted` | The flag is set by the script: no second gift; `done` at the next save. |
| A disconnect at any point | The prompt stays through the 30 s grace (the relay resends the offer to the partner when it reconnects); an answer given offline is sent on reconnect. Nothing is applied by the network. |
| A malformed ledger | Moved to `<save>.scene-quarantine`, starts empty. Consent answers are lost; the flag still prevents a second gift. |

A relay restart loses only open offers; there is nothing to recover there.

## Wire changes (protocol 11)

New opcode `Y`. All integers little-endian.

| Subtype | Body bytes | Direction | Contents after the subtype |
| --- | ---: | --- | --- |
| 0 mode | 2 | client → relay → peer | mode u8: 0 Social, 1 Shared, 2 Shared with this session's consent |
| 1 offer | 12 | initiator → relay → partner | offer id u64 (random), scene id u16, nearby u8 (0 Everyone, 1 Nearby) |
| 2 answer | 10 | partner → relay → initiator | offer id u64, answer u8: 1 Ready, 2 Not Yet, 3 Stay Out, 4 already completed, 5 not sharing |
| 3 cancel | 10 | initiator → relay → partner | offer id u64, how u8: 1 Cancel, 2 Continue Without |
| 4 closed | 10 | relay → client | offer id u64, reason u8: 1 not both sharing, 2 not nearby, 3 another offer is open, 4 no partner, 5 expired (60 s), 6 initiator gone (30 s grace, leave or removal), 7 no such offer |

The relay (`scene_action` in `server/coop_relay.py`) keeps one open offer per room. It sends each peer's last mode to a
newcomer, resends the open offer to its partner when it reconnects, and closes it when the initiator hasn't come back
after `RECONNECT_GRACE` (30 s). An older relay answers `Y` with "unsupported message" and closes the connection, so the
client and relay must not be mixed: `Y` came with protocol 11. No existing message changes.

## Files

| File | Role |
| --- | --- |
| `runtime/coop_scene.c/.h` | The scene table, the checks, the prompt state, the `Y` messages and the ledger. No game calls. |
| `runtime/hle/coop_scene_apply.c` | Game thread: the flag polls, the apply, the save-image hook. |
| `runtime/ui/coop_scene_ui.mm` | The prompt, the initiator's wait and result, notices, the HUD mode line, the settings and Story scenes. |
| `runtime/coop_net.c/.h` | `coop.ini` (`adventure=`, `scene_offer=`), the `Y` channel, consent cleared per session. |
| `runtime/services/fs.c` | `coop_scene_recover` when the save opens, `coop_scene_after_replace` after each whole-save write. |
| `runtime/hle/coop_actor.c`, `runtime/main.c` | `coop_scene_field` on overworld updates, `coop_scene_configure`. |
| `runtime/ui/menu.mm`, `runtime/ui/coop_ui.mm` | The overlay, input capture, HUD line, Friends and launcher settings. |

Not done: the native Communicate menu has no Shared Adventure page (Friends only).

## Tests

`tests/scripts/run-scene-tests.sh` builds and runs both; the log is `tests/logs/scene-tests.log`.

- `tests/tools/scene-test.c`: the table's hashes, personal flags refused (badges, gyms, rival, League, trainers),
  story-point checks, Social mode and missing consent never apply, Ready / Not Yet / Stay Out, cancel and Continue
  Without, replayed and repeated offers, repeated catch-up, disconnect and expiry, every interruption in the table
  above with a fake game and save, quarantine, and the initiator's side.
- `tests/tools/scene-relay-test.py`: modes, the gate (sharing, Nearby, tagged), one offer at a time, answers, stale
  answers, cancels, expiry, the partner's reconnect (resent offer), the initiator's return within the grace and its
  absence after it, leaving.

Fixtures for the live checks: `tests/saves/shared-adventure/` (README.txt).
