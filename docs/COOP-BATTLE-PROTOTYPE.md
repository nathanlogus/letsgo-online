# Next native battle prototype

The protocol-5 social tag is an invitation/consent foundation. It does not enable a shared battle. Existing social party/mode guards remain in place until the following native paths are implemented and verified.

## Findings from the 0.7.3 integration

- `runtime/hle/coop_actor.c` intercepts native P2 party selection at `0x70002462e0` and battle consumers of controller mode `0x7000142720`. Removing those guards would select another Pokemon from the local save, rather than the guest's party.
- Upstream `wildbattle.c` now maps the top-level battle menu through `CMD_DECIDE` (`0x7000ab1e80`), `CMD_OWNER` (`0x7000ab1670`), views and button callbacks. The decide function reads a UI option ID and changes the owner's action at `owner+0x78`, type at `action+0x10`. This is the Fight/Bag/Pokemon/Run entry point; it is not a complete move/target command and must not be treated as one.
- Local classic combat configures the static battle at `0x700028c770`, its context at `0x70009d2ce0`, and native post-battle registration at `0x700031a400`. These are wild/static paths; the ordinary trainer factory is now mapped separately below.
- Both implementations previously registered some of the same hooks. The integration initializes the selected classic mode before wild-battle registration and keeps upstream CATCH/GO hooks in the alternate mode. Shared-battle work must preserve that separation and the ordinary trainer path.

## Required first playable slice

1. Observe an ordinary trainer battle: completed move/target/switch selection, party getters, turn number, RNG initialization and result/reward consumers. Keep observations local while identifying their ownership.
2. Construct a temporary guest combatant from explicitly exchanged guest party data. It must have separate ownership and never become a second member of the host's saved party. Do not copy a complete save or raw guest-memory pointers.
3. Select one authority for the battle. Bind every command to a battle ID, turn, trainer slot and sequence; reject duplicate, stale, out-of-turn and non-owner commands. The guest supplies its own decisions, not host P2 button presses.
4. Establish identical initial state and synchronized presentation on the guest. Compare a normalized battle-state digest at turn boundaries. A menu choice alone is insufficient for replication.
5. Start with one ordinary trainer, a copied save per client, no shared story-completion flags and explicit per-player reward rules. Test decline, cancellation, disconnect before/after a turn and repeated results before applying rewards to either save.

Trades, shared story ledgers, PvP and spectating depend on this ownership/authority work. They are not unlocked by accepting the current social tag.

## Native party and trainer preflight (25 October 2026)

`runtime/coop_party.{h,c}` defines a pointer-free party snapshot: a count of 1-6 and 260 bytes per native serialized PB7. Validation checks the encrypted core checksum, bad-egg flag and calculated level/HP/stat bounds. A digest supports comparisons; it is not authentication or a complete battle-state digest. Normal social play sends no party data. The opt-in battle extension described below exchanges only an accepted guest lead.

`runtime/hle/coop_battle.c` runs opt-in, local QA on Eevee 1.0.0. The native party serializer (`0x7000729650`) and importer (`0x7000729660`) round-trip six Pokemon from a different copied save. Temporary objects use the native constructor/destructor, never the saved-party insertion or capture-registration path. Native serialization of the local party is compared immediately before/after import and remained identical. The exported bytes also decode correctly in PKHeX.

Ordinary trainer process factory: `0x700028bb60`, receiving a 0x30-byte configuration. Its trainer fields are **ally, opponent 1, opponent 2**, in that order. A simple solo trainer configuration is `{0, 0, trainer_id, 0, ...}`. Putting the trainer in the ally field leaves the enemy party empty. Trainer 26 loads a level-5 Rattata; a copied-save automated run opened Fight, selected a move and returned to the overworld at game frame 3875. A screenshot verified the move menu. The initial return capture exposed a black fade: the field callback resumed, but the skipped story script had not performed FadeIn. The QA helper now schedules that native FadeIn 30 frames after return. Final solo and foreign-lead runs visibly returned to the overworld in frame-4200 captures; their field callbacks resumed at frames 3475 and 3875 respectively. This test does not run a trainer's overworld story script.

The top-level choice observer composes with the existing hook through `rt_register_fnhook_chain`. It copies the choice while the UI object exists; the object may be destroyed when its submenu opens. Do not retain its pointer across frames. This top-level observer does **not** capture a complete move/target decision; the separate native result-getter observer below handles ordinary moves.

QA controls (all output directories must already exist):

- `LARECOMP_COOP_PARTY_TEST_DIR`: export `party.bin`, `import.txt` and local battle journal. Fixture format is `LGPTY001`, one count byte, then count x 260 serialized bytes.
- `LARECOMP_COOP_PARTY_TEST_PEER`: a local fixture from a second copied save.
- `LARECOMP_COOP_TRAINER_TEST_AT`: start the trainer factory on an overworld update at/after this game frame; requires an explicit save directory and party-test directory.
- `LARECOMP_COOP_TRAINER_TEST_ID`: ordinary trainer ID, default 26.
- `LARECOMP_COOP_TRAINER_TEST_GUEST`: experimental multi-trainer setup with the foreign lead imported into the battle-owned ally party. The multi-trainer configuration fills both opposing slots, which the native mode requires. The foreign level-50 Machamp rendered alongside the host's level-100 Eevee. This investigates ownership and presentation only; the native NPC ally still controls its decisions.

Fixtures, copied saves, test binaries, scripts, logs and captures stay under ignored `tests/`. Checksum/count/level/HP rejection tests and hook-composition/reinstallation tests pass. Real two-client battle authority, completed command ownership, RNG/state replication, disconnect recovery and reward rules remain pending. The normal social tag still keeps battles individual.

Final copied-save checks: the host retained all six original species, encryption constants, personality IDs, trainer IDs and forms after both battles. Checksums remained valid. The host lead consumed one PP normally; the foreign Machamp was not inserted into the host party. The two original manual-test save SHA-256 hashes remained unchanged. These checks establish temporary ownership for this local prototype, not network command synchronization or a final shared reward policy.

## Move/target command boundary and authority gate

The native move picker is `0x7000aca930`. Its result records the selected move ID, while `0x7000aa5660` converts it into a native command. Target selection is completed separately by `0x7000aa5ca0`: return 0 means pending, 1 means committed, and 2 means cancelled. This getter updates the command through the pointer at `ui_root+0x320`. For an ordinary move the low nibble is action kind 1, bits 4-7 are the mapped target, and bits 8-23 contain the move ID. Higher bits carry additional native flags and must be preserved/researched before supporting special actions.

The opt-in observer copies the completed value into `battle.jsonl` at this getter. It records move slot, move ID, target and actor, without retaining native UI pointers. `observed_sequence` counts local observations only; it is not a verified native turn number or an accepted network sequence.

`runtime/coop_battle_command.{h,c}` provides a separate, pointer-free two-player turn barrier. Commands carry battle ID, turn, sequence, trainer slot, party slot, move slot/ID and target. Sender identity is supplied separately by the authenticated session. The authority supplies current moves, PP and legal target masks. It rejects wrong-player slots, stale battles/turns, sequence gaps/replays, unavailable moves and invalid targets without changing state. Both allied trainers (slots 0 and 2) must submit before a turn is released in canonical order. Disconnect aborts and clears pending commands; completed or aborted gates cannot reopen.

This two-human-command gate is tested in isolation. The opt-in guest-control extension below uses a separate per-ally deadline gate with native host input; it does not yet replicate both clients' battle engines. Presentation replication and reward handling remain prerequisites for a complete synchronized battle. Switches, items, special actions and forced moves are not implemented in this first command slice.

Automated copied-save command checks:

| Scenario | Cancellation observed | Completed moves | Mapped target |
| --- | --- | --- | --- |
| Cancel target, choose again | `target_cancelled` before any command | 1 Double Kick | 3 |
| Cancel move menu, choose again; battle continues to another turn | `move_cancelled` before any command | 2 Double Kicks, on separate menu decisions | 1 |
| Choose the other target | None | 1 Double Kick | 1 |

The two final runs consumed exactly two and one Double Kick PP respectively. Cancelling did not consume PP or increment the observed command sequence. Frame-5400 captures verified return to the visible overworld. All original host party identities remained intact and both original manual-test save hashes were unchanged. Recorded native commands also pass the isolated authority-gate replay check, including waiting for both decisions, duplicate rejection and monotonic sequences across multiple turns. This replay check supplies a synthetic second decision; it is not a two-client battle test.


## Two-client guest move injection and NPC timeout

The Eevee 1.0.0 controlled trainer trial now exchanges a live guest lead after explicit consent and accepts the guest's move/target over the existing relay. The host owns the native battle engine. The guest remains in its own overworld with a temporary decision panel; native battle presentation is **not** replicated to the guest yet. This is an opt-in QA trial, not automatic participation in ordinary story battles.

Enable `LARECOMP_COOP_GUEST_CONTROL=1` on both clients and use the updated relay. The existing copied-save trainer test controls still start the trial. `LARECOMP_COOP_ALLY_TIMEOUT_SECONDS` defaults to **30**, clamped to 1-60. The host displays the ally's name, countdown and a waiting bar. The guest sees move names and PP from its battle-owned combatant, then named opponents with current HP. A sole legal target is selected automatically. Buttons support mouse, keyboard and the selected Nintendo/Xbox controller layout. Back returns from target selection; Back/Escape declines an invitation. A new prompt explicitly focuses the first valid action and reveals the navigation cursor.

At the deadline, the existing native NPC chooses **only that turn**. The battle continues, and the next eligible turn creates a fresh guest prompt. Late, duplicate, stale, wrong-owner, exhausted-move and invalid-target commands are rejected. Losing the guest connection releases the current wait and leaves the native NPC in control. Unacknowledged offers also expire locally after 45 seconds. Decline does not repeatedly reopen the QA invitation.

Native boundary: ally controller `0x7000a577f0`, trainer slot 2, phase 2. Its AI job must be advanced with `0x7000a6d310` and checked with `0x7000a6d230` before deferring native command consumption. Phase 0 falls through initialization in the same call, so waiting at phase 1 misses the decision. A valid guest decision uses `0x7000a50a90` and resumes native phase 3. Forced/fainted actions, unsupported targeting and replacements retain native handling. The current slice supports the imported lead's ordinary selected-opponent moves only.

The native position getter `0x70009d9d50` already returns absolute battlefield positions. Applying the default-opponent mapper again targets our own side. The trial's borrowed trainer AI can also choose a friendly position: the guarded NPC result hook at `0x7000a5c9a0` preserves its chosen move but normalizes an invalid selected-opponent target to a living enemy through `0x7000a50ac0`. These corrections are limited to the opt-in trial.

Verified automatically on copied saves:

- `ally-control8`: five-second missed turn used native NPC Sappy Seed against opponent 1; guest Bite targeted the remaining opponent 3 on the next turn; host returned to the visible overworld. Captures verified undamaged allied HP and the waiting message.
- `ally-ui1`: ordinary SDL keyboard navigation submitted a guest decision after a missed first turn; native injection and overworld return were journaled.
- `ally-ui3`: ordinary SDL controller navigation selected Bite and the second target; native journal recorded move 44, target 3. Disconnecting the guest afterward let the next native NPC turn finish and return to the overworld. Captures show real move names, PP, target HP and countdown.
- Pure deadline/link tests and socket relay tests cover exact deadlines, exactly-once fallback, guest recovery next turn, stale/late/duplicate rejection, role and consent checks, offer expiry and disconnect cleanup. Existing social invitation tests still pass.
- Host party comparisons retained all six identities and valid checksums; only the two normal Growl PP were consumed. The guest lead was never inserted into the saved host party. Original manual host/guest saves retained their SHA-256 hashes.

QA tools can write relative SDL actions to `LARECOMP_HOST_INPUT_FILE`, for example `+10:sdlpad:east:6` for Nintendo confirm or `+10:sdlpad:south:6` for Xbox confirm. `+10:shot:guest-choice:1` captures the composed window under an explicit `LARECOMP_CAPTURE_DIR`; simple names only. These inputs and captures are disabled without the test environment variables. Copied saves, automation, journals, logs and images remain in ignored `tests/`.

Still open: guest native battle scene/state replication and turn digests; ordinary trainer entry integration; switching, items and special targeting; agreed per-player EXP/PP/HP/reward persistence. Current guest saved HP, PP and rewards are not modified. Shared story flags, gyms and badges remain independent. Do not enable cross-save reward or progression writes as part of this trial.


Input regression found during full SDL testing: a confirm could hide an overlay on key-down, causing its later key-up to bypass ImGui. The next prompt then ignored that button because it was still logically held. Keyboard and controller releases now reach the overlay even after capture closes, and inactive overlay frames preserve them. This applies to Friends/settings as well as battle prompts.

Party wire validation now decrypts the four shuffled core blocks to bound species, move/relearn IDs, PP and PP-Up values before native import. Tests cover all 32 shuffle selectors plus checksummed invalid fields; this is structural validation, not a complete legality checker. Format offsets were checked against [PKHeX PB7](https://github.com/kwsch/PKHeX/blob/master/PKHeX.Core/PKM/PB7.cs) and [its encryption implementation](https://github.com/kwsch/PKHeX/blob/master/PKHeX.Core/PKM/Util/PokeCrypto.cs).


## Repeated native state tracing (26 October 2026)

The controlled host engine has a context-local xoroshiro RNG object at context `+0x90`, with state words at `+0x98/+0xa0`. Context update `0x70009d2ce0` initializes it through `0x70009e8a50`; explicit-seed initializer `0x70009e8a80` delegates to `0x7000145400`. Setup flag byte `+0x67 == 1` instead selects the seed already stored at setup `+0x528`. This mapping comes from the user's extracted Eevee executable.

`LARECOMP_COOP_BATTLE_SEED` optionally fixes that context-local seed for QA. A thread-local initialization guard excludes world and AI RNG objects. No seed is changed without that explicit test variable and the existing isolated trainer trial. The journal records `battle_seed` and a `turn_state` at each eligible ally decision. Snapshots contain only copied values: two RNG state words and trainer-ordered lead species, HP/max HP, moves and PP. Their FNV digest excludes pointers and frames. This is a **partial diagnostic fingerprint**: it omits status/stages, weather, reserves, secondary RNGs and engine scripts, so it must not be used as a complete desync proof.

Two separate two-client runs, `ally-seed1` and `ally-seed2`, fixed the seed to `0123456789abcdef`, used the same missed first choice and guest second choice, and returned to the overworld. Both recorded exactly matching snapshots, including RNG and combatant values, with decision-1 digest `eef083c2209a1a9b` and decision-2 digest `5ec6fa85dcd9b694`. The host's original party identities remained intact and the two ordinary Growl PP were consumed. This establishes repeatability for this controlled case; it does not yet run a guest battle engine.

The native temporary-party constructor is `0x7000720f10`; it creates six empty native Pokemon objects. A party allocation is 0x90 bytes with alignment 8 and the same allocator/self/size/alignment metadata used by native allocation sites. Holding/releasing the shared reference at party `+0x48` lets the native destructor own nested Pokemon and the outer allocation. The preflight now imports/re-exports all six supplied Pokemon in a temporary native party and releases it, while comparing the local party before/after. `ally-ui-smoke5` passed this ownership preflight together with all guest UI cases.

The repeatable one-native-client UI harness also passed Escape and Xbox-B decline, Enter join, exact two-chunk lead exchange, A select/B back, analog-stick target selection, repeated prompts, a timeout toast and a fresh guest choice next turn. Its host peer is synthetic; the separate two-native-client runs establish actual command injection and NPC continuation. Artifacts: `tests/runs/ally-ui-smoke4` and `ally-ui-smoke5`. Native Nintendo-layout (`ally-ui7`) and Xbox-layout (`ally-xbox1`) battle runs additionally verified target 3 injection and normal overworld return.

## Guest replay prerequisites and experimental bridge

The isolated `battle-p0-clone8` trial used a six-member temporary host party while preserving the local three-member party byte-for-byte at command selection, after battle and in a later overworld snapshot. Redirecting the battle pool alone was insufficient: the native after-battle process (`0x7000319bc0`) copies results back, and a separate Spirit setter (`0x7000737490`) modifies the partner. The trial bypasses the former result phase and guards the latter by saved backing-buffer identity. These guards are scoped to the temporary-party trial. The temporary party consumed normal move PP.

`battle-p0-remote1` additionally injected the main player's command before the human move menu opened, completed through the native controller path and returned to the overworld with the same local-party isolation checks passing.

The opt-in `LARECOMP_COOP_BATTLE_MIRROR=1` extension adds host-party chunks (B7), the initial battle seed (B8) and committed commands (B9). Host ownership, consent, bounded chunk indices, per-actor command sequences, duplicate rejection and authoritative ally choices have passing link/socket tests. Normal battle-end delivery retains queued guest replay commands until consumption. The native bridge now builds in `tests/build-coop-mirror`; full two-engine replay and matching combat state remain under test. This is not enabled for ordinary social sessions, and it does not authorize progression or reward writes.

The first live two-engine run, `mirror-native1`, completed on both clients and restored both overworld views. Both turn snapshots matched exactly (including the diagnostic RNG, HP, moves and PP), with digests `eef083c2209a1a9b` and `5ec6fa85dcd9b694`. The first guest deadline fell back to native AI; the second turn accepted guest Bite. The guest replay consumed both the host and ally commands, including the AI result path. Its local party before/after was byte-identical. The guest result-screen capture was inspected. Evidence is in `tests/runs/mirror-native1/verification.json` and the two journals. This verifies one controlled two-turn encounter, not general battle determinism, reward isolation, switching, items or ordinary trainer entry.

## Guest save isolation and replay abort (26 October 2026)

`tests/tools/run-mirror-save.ps1 -SaveCheck` has each client save in game (X, Save) before the battle and again after it, copies both files and compares them with `party-fixtures savediff`: every save block's changed bytes plus money, bag, party (EXP, level, HP, PP, status), Pokedex counts and event flags/work. Only PlayerGeoLocation, PlayTime and Coordinates (position and field objects, rewritten by any battle) are allowed to differ. The fixtures are prepared copies in `tests/saves/coop-mirror-fixtures`.

- Host (positive control, real battle): money 47968 -> 50768, Growl PP 40 -> 38. The check sees real rewards.
- Mirrored guest (`mirror-save3`, `mirror-save4`) and aborted guest (`mirror-abort1`): money, bag, party, Pokedex, flags and work values unchanged. No mirrored reward reached the guest save; the existing result-phase and partner guards hold.
- CaptureRecord +0x7A4 (an unnamed u32 after TotalCaptured) goes 31 -> 32 on both clients when the social P2 trainer joins, with or without a battle (`coop-nobattle2`). It comes from the social join, not from the battle. It is game record 0x1E9 (the record array starts at the block; +0x7A4 = 0x1E9 * 4), the native 2-player join count: the controller-mode update `0x7000142840` calls the record add `0x70001cad40` with that id when P2 joins (found with `LARECOMP_WATCH`, `coop-record2`). `coop_actor.c` skips that id while online; `coop-record3` keeps 31 on both clients. The pre save comes after the join, so compare the fixture with `save-post.bin` (`save-diff-fixture.txt`); EventWork 675 (92 -> 0) also changes on a solo load (`savecontrol2`).
- The in-memory dump (`LARECOMP_COOP_SAVE_DUMP`) is the load buffer. It never changes, so it cannot show leaks. Use the in-game save comparison instead.

Turn digests now include the five status conditions and a raw hash of each battle Pokemon (+0x78..+0x2e0, skipping 8-byte slots that hold guest pointers, so stat stages and volatile counters are covered). The old fields are journaled as `partial_digest`. Field effects (weather, terrain, screens) are not covered yet. Both sides report the digest through `coop_battle_link_mirror_digest`. A mismatch, a link abort, an incomplete end or a lost link journals `mirror_aborted` with its reason once. The guest then stops taking replay commands and lets the native AI finish its local copy, with results and partner writes still discarded. The host logs `mirror_aborted_host` and keeps its battle. `LARECOMP_COOP_MIRROR_CORRUPT_ROUND=<n>` corrupts the guest's report for one round (QA only). `mirror-abort1` aborted at round 1, returned to the overworld and passed the guest save check. One earlier run also mismatched on the extended digest without injection. Its raw data was pruned, so `LARECOMP_COOP_TURN_RAW=1` now writes `turn-raw.txt` for the next case.

Ownership check (`mirror-ui1`, guest driven through its own prompt by `tests/tools/drive-mirror-ui.py`): the host's native Fight menu decided only trainer 0 (`move_committed` actor 0). The guest prompt listed only the guest lead's moves; turn 1 was the guest's Bite on target 3 (`ally_injected`). Turn 2 timed out, the NPC chose that turn only, and the guest replay applied the same commands. Both sides' digests matched.

## Native battle menu for the guest (26 October 2026)

In the mirror trial the guest now chooses in the game's own battle menu (Fight -> move -> target) for its slot-2 Pokemon, and each player waits in the native battle message window ("Waiting for <partner name>...") while the other has not chosen. `LARECOMP_COOP_BATTLE_OVERLAY=1` keeps the overlay panel and waiting bar; the overlay also returns if no battle UI client is found.

- Local multiplayer is not the way in: the controller factory (`0x70009ef8e0`) makes exactly one human controller, for the local slot (`context+0x18c`, from setup `+0x66`); Let's Go's local P2 gets a second active Pokemon inside trainer slot 0 (the human step `0x7000a5a180` loops over `0x70009d9b80` Pokemon), not a slot-2 menu. Controller type is `+0x1fb` (0 human, 1 AI, 2 link); the per-kind handler table is `0x70015d0c68` (kind 1 = action select: human `0x7000a574f0`, AI `0x7000a577f0`, link `0x7000a57be0`).
- The guest runs the human action handler `0x7000a574f0` on its slot-2 AI controller with its own state, after the AI job and digest of that turn, lending it the battle's single UI client (`context+0x1a8[context+0x18c]+0x280`) through the controller setter `0x7000a57000`. The controller fields the human flow writes (`+0x1e0..+0x27f`, `+0x288..+0x2af`, `+0x3b8..+0x3cb`) are snapshotted and restored, so the replay still applies only the host's canonical slot-2 command. The chosen move/target goes to the host as before (op 4). A target cursor left on a fainted foe is mapped to the only remaining foe; Bag/Pokemon or an unsupported move reopens the menu.
- Deadline: the native link-battle time limit closes the menu (`+0x260` nonzero, `+0x200` time up; every human sub-step checks them and the time-up step `0x7000a5a340` closes the menu through UI `+0xc0` and auto-picks). It fires at the guest deadline, when the host's command arrives first, on abort or when the link stops waiting. The host's NPC then chooses that turn and the menu opens again next turn.
- Waiting message: UI vtable `+0x1b0` opens a battle message from a 0x24-byte parameter (`0x7000a6f000` layout: u16 line, type 1 = btl_std). btl_std line 201 (a link-battle-only "The opposing player's time has run out!") is shown, and its decrypted text (`0x70006786b0`) is replaced with "Waiting for <CoopRemote.name>...". Host: once its slot-0 handler returned 1 and the guest has not answered. Guest: once its slot-2 command is in and the host's slot-0 command is not.
- The turn digest skips battle-Pokemon bytes the game never sets (`raw_unused`), since they keep whatever that heap
  memory held before and differ per client: setup padding (`+0x93..+0x97`, `+0x22b`, `+0x22f`, `+0x239..+0x23f`,
  `+0x25a..+0x25f`, `+0x27b`) and the 24 one-byte slots of the set at `+0x212` (count) / `+0x213` past its count.
  This replaced the earlier `+0x218`-only skip (bit 3 there varied per run). Protocol 8, since a digest of the old
  rule never matches the new one. See "Round-1 digest mismatch" below.

Verified (copied saves, `tests/tools/run-mirror-save.ps1 -UI -Timeout 20 -Driver drive-native-menu.py`, game input via `key:` presses): `native-ui7` - guest captures `guest-menu-t1.png` (native Fight/Pokemon/Bag), `guest-timeout-t1.png` / `guest-after-timeout-t1.png` (menu closed, NPC chose, battle continued), `guest-menu-t2.png`, `guest-moves-t2.png` (Machamp's own moves and PP), `guest-targets-t2.png`, `guest-waiting.png` ("Waiting for Host..."); host `host-waiting-1.png` ("Waiting for Ally..."). Turn 2 guest Bite reached the host (`ally_injected` 44/1), both digests matched, both returned to the overworld, guest party unchanged. `native-ui6 -SaveCheck`: guest save PASS, digests matched; an earlier aborted run (pruned) closed the open menu cleanly and its guest save also passed.

## Normal play (27 October 2026)

The trainer team-up (ordinary trainers, mirror mode, native guest menu, partner avatar, 30 s deadline) no longer needs QA variables: the hooks install on every Eevee 1.0.0 boot and pass through unless a team-up runs, the battle channel and the guest replay are on by default, and the setting "Team-up battles" (`coop.ini`, default on) turns offers off. Protocol 6. Journals, party files, fixed seeds, fault injection, auto accept/move/tag, the factory trigger and save dumps stay behind their variables; see [COOP-ONLINE.md](COOP-ONLINE.md#team-up-trainer-battles) and [COOP-TRAINER-ENCOUNTERS.md](COOP-TRAINER-ENCOUNTERS.md#8-normal-play-27-october-2026).

## Switches, replacements and items (27 October 2026)

Roadmap priority 4. Each player now controls only their own Pokemon for every native decision, not just moves. Protocol 7.
Eevee 1.0.0 addresses; decompiled with Ghidra headless (`tests/cache/ghidra/decomp.sh <out> <addr>...`, ignored).

### How a controller decides

- The battle server sends each trainer controller a request. The request id is at controller `+0x21c`; the
  dispatchers `0x7000a55bf8` / `0x7000a56760` pick a handler from the table `0x70015d0c68` (0x38 bytes per entry:
  request id, then human, NPC and link handlers). Request 2 is the action menu (human `0x7000a574f0`, NPC
  `0x7000a577f0`). Request 4 is the switch-in after a faint and request 5 the mid-turn one (human `0x7000a57ff0` /
  `0x7000a584f0`, both `0x7000a5d470`; NPC `0x7000a58000` for both).
- A handler that returns 1 has answered. Its reply is `+0x220` -> `+0x230`: a count byte and one u32 command per
  active Pokemon (0x2c bytes). One trainer has one active Pokemon in a team-up, so the count is 1.
- Command word, low nibble = kind:
  - 1 fight: bits 4-7 target, 8-23 move, bit 27 a flag (`0x7000a50a90`).
  - 2 item: bits 4-6 position, 7-22 item, 23-30 an argument (`0x7000a50c30`).
  - 3 change: bits 4-6 the position, 7-9 the battle-party slot (`0x7000a50c50`); `0x403` means no change (`0x7000a50c70`).
- Request 4 goes to every controller after any faint. A controller answers for the positions it owns: the
  request lists them (`0x7000a51c70`) and `0x70009d9f00` gives each position's trainer. A trainer with nothing to
  replace answers command 0; one with no Pokemon left answers `0x403`.
- The party list shows "its" party from the UI's owner controller (UI `+0x60`, read by UI `+0x6b8` through
  `0x7000a64700`), not from the controller it serves. The UI is built for the local slot by `0x70009d57d0`
  (UI constructor `0x7000a77cd0`, vtable `0x70015d3268`).

### What the team-up does with it

- **The guest's whole party is the ally party.** B2 sends the guest's party, up to six, as `count, chunk index`.
  `trainer_party_hook` imports it into the trainer's battle party (slots `0x7000721b00`, count `+0x88`). It does
  this on the host from the received party and on the guest from the party it sent. So the ally sends in only the
  guest's own Pokemon. The party is copied into the battle and never written back.
- **Every decision is published as its command word.** B9 (30 bytes) carries the request kind, the reply count
  and the first command. Both clients count every decision of a slot in the same order: each kind-2 menu round
  of slot 2, each switch-in request of slot 2, and every slot-0 handler completion. The host publishes:
  - slot 0 when its handler returns 1 (`human_select_hook`, `switch_in_hook`), forced actions included;
  - slot 2 when it injects the guest's move or switch, when the NPC commits (`npc_commit_hook`), and from
    `ai_switch_hook`.

  The guest's slot-0 handlers take the host's decision before their menu (step state 2 for moves). For forced
  actions they take it at completion and reply with the host's command. A request whose kind differs from the
  host's aborts the replay. The host's moves, items and switches are replayed as they are; only the guest's own
  moves are checked against its Pokemon.
- **Guest switches.**
  - B3 (54 bytes) adds the request kind and a mask of the party slots the guest may send in: past the trainer's
    active ones (`0x70009daa20`) and not fainted (`0x70009e3a20`).
  - B4 (23 bytes) adds `switch_to`, a slot + 1.
  - The guest's own native menu (`native_menu`) now also handles *Pokemon* in the action menu and the switch-in
    party list (requests 4/5). While the UI is lent, its owner (UI `+0x60`) is the slot-2 controller, so both lists
    show the guest's party with the slots the commands use.
  - The host accepts only an offered slot (`coop_ally_submit`), and so does the relay (B4 and B9 against the B3
    mask). It builds the command itself: `0x7000a50c50` with the slot for an action-menu switch. For a switch-in,
    the NPC handler runs once on both clients and its command's slot bits are replaced by the guest's slot.
- **Timeouts.** A switch-in waits for the guest under the same ally deadline (`LARECOMP_COOP_ALLY_TIMEOUT_SECONDS`,
  default 30). After the deadline the NPC's pick stands for that request only (B5 closes the guest's list).
  Nothing is asked when the guest has nothing to replace or no Pokemon left. Then the native multi battle goes on
  with the host alone.
- **Items.** The battle spends an item from the local bag only for the local slot (`0x70009da170` ->
  `0x70001c9490`). So the host's own Bag works normally and spends from the host's bag. The guest replays the
  host's item command, and its bag removal is skipped (`item_use_hook`). The guest's Bag is refused: the action
  dispatcher `0x7000a5a810` asks `0x70009e3e50(Pokemon, 0x22)` before opening the Bag. When it holds (the call that
  returns to `0x7000a5a918`), the game refuses natively and the action menu comes back. `bag_check_hook` makes it
  hold while the guest's menu is lent. A guest item would have come from no one's bag on the host. An item command
  that still reaches the guest's menu brings the menu back.

### Verification (copied saves, two clients, `tests/scripts/run-teamup-play.sh` + `teamup-start.sh`)

Fixtures: `party-fixtures setmon <in> <out> <slot> <hp> <status> [move1] [level]` sets a lead to level 2, HP 1 and
Double-Edge, so its recoil knocks it out without knocking out a Kakuna (`tests/saves/teamup-switch/`).
`NOWILD=1` (`LARECOMP_WILDBATTLE_NONE`, test-only) stops wild Pokemon from touching the idle clients. `QA=1` adds the
journals. `RAW=1` writes each round's battle-Pokemon dump (`LARECOMP_COOP_TURN_RAW=<file>`).

| Run (`tests/runs/`) | Covered | Result |
| --- | --- | --- |
| `teamup-switch2/guest-faint` (QA) | Host switch on turn 1 (`0x83`); guest lead knocked out by recoil; the guest picks its replacement in its party list (`a_g_replace*.png`); the NPC's turn, then the end | Guest slot 3 (`0x183`) replayed; digests of rounds 1 and 3 matched; the guest's Charizard Lv 50 is in on both (`a_t3.png`). Guest save orig/post: only the known load noise. This run predates the UI-owner fix, so its lists showed the host's party, with correct slots. |
| `teamup-switch2/both-faint` (QA) | Both leads knocked out on turn 1; the host picks natively, the guest in its own list (`b_repl.png`, guest Lv 50 party); turn 2: guest action-menu switch (`b_g_list.png`, `0x103`) and host Revive on its fainted Eevee (`b_h_*.png`, `0x7f800e02`) | All decisions replayed; rounds 1, 3 and 4 matched; `mirror_item_use_discarded` on the guest. Host save: Revive 30 -> 29, Eevee revived (7/15), money +36, 3 Poke Balls, flag 2742. Guest save: bag, money and party unchanged (known noise only). |
| `teamup-switch2/guest-out` (QA, 15 s deadline) | Guest party of two level-2 leads; first knocked out; replacement list (`c_g_repl.png`) left unanswered; second knocked out; host alone to the end | The NPC took the replacement after 15 s (`0x83`, NPC), the menu closed. The second faint asked nothing (mask 0, `0x403`). The battle went on with the host alone (`c_now.png`) and ended normally. Rounds 1, 2 and 4 matched. Guest save: party 2 -> 2, unchanged. Host: +36, 3 Poke Balls, flag 2742. |
| `teamup-switch3/final` (no QA variables, `RAW=1`) | Both knocked out; host native pick; guest own list; guest action-menu switch (`e_g_list.png`); host Revive; guest Bag refused (`e_g_bagB/D.png`: no Bag, the menu returns); request 4 when only the host's Pokemon fainted asked the guest nothing | Rounds 1, 3 and 4 matched, and the guest finished its replay. Both windows were closed from outside at t = 592 s, before the post-battle save. |
| `teamup-switch3/raw` | A request 4 for a guest with nothing to replace | Found the "asked anyway" bug: the guest's list reopened until the deadline. Fixed with the position check above. |

Unit and relay tests: `tests/scripts/run-battle-link-tests.sh` (all pass). They cover the switch and replacement
choices limited to offered slots, the full-party B2, B9 kinds and commands, and relay rejection of unoffered slots.

### Known issues

- After a guest abort (B10) the host's NPC no longer chooses only at the deadline: the lost replay is noticed on the
  wait (`host_replay_aborted`) and the NPC decides at once, for moves and for switch-ins, for the rest of the battle.
- After an abort the guest finishes its local copy natively. A faint of the host's Pokemon there used to open the
  replacement list on the guest for the host's side (`teamup-desync3/abort2`); the NPC handler now picks it
  (`switch_in_hook`, see "Partner rewards" below).
- The overlay (`LARECOMP_COOP_BATTLE_OVERLAY=1`) lists moves only. A replacement there falls to the NPC at the deadline.
- (Fixed in protocol 10.) The guest's own moves had to target one opponent (`ally_choices`): Growl and other spread
  or self moves were taken back by its menu (`teamup-midturn2/host-uturn`). Every target type is now offered; see
  "The partner's move targets" below.
- The guest's replay presses A through the battle text, the host's Pokemon's level-up included. A host Pokemon that
  learns a move with a "forget a move?" choice makes the two engines differ when the host keeps its moves or forgets
  another one than the first: the replay then aborts, and the partner gets no rewards for that battle. A host
  Pokemon gaining EXP at all broke every replay before protocol 10 (two causes, fixed: see "Replay fixes found by
  the wild team-ups" in [COOP-TRAINER-ENCOUNTERS.md](COOP-TRAINER-ENCOUNTERS.md#11-wild-team-ups-28-october-2026));
  the earlier runs had level-100 hosts.
- Only the first command of a reply travels. That is enough for one active Pokemon per trainer.

### Round-1 digest mismatch (28 October 2026)

`teamup-switch3/normal` and one earlier run (2 of 9) aborted the guest replay at round 1 with otherwise identical
RNG and fields; their data was pruned. `RAW=1` dumps now also show each skipped byte (` unhashed <value>`).

- Cause: battle-Pokemon bytes that no code writes. In `teamup-desync2` (9 two-client runs, `RAW=1 NOWILD=1`,
  `tests/scripts/teamup-desync-loop.sh`) host and guest differed at round 1 in 3 runs, always in the second
  trainer's (an opponent's) slots past the `+0x212` count, which was 0: `+0x218` bit 3 (`r1`, `r9`) and `+0x215..+0x216`
  (`r3`, `00000072da` on one side, 0 on the other). Other trainers held leftover heap words there (e.g. `0x7000f1484e`).
  The old `+0x218`-only skip would have aborted `r3`.
- With the skip list, all 9 runs matched at round 1 (`summary.txt`), and no hashed byte differed in any dump.
  `teamup-desync3/save` (no QA variables) matched as well.
- Forced abort (`CORRUPT=1`, `LARECOMP_COOP_MIRROR_CORRUPT_ROUND`, `teamup-desync3/abort3`): the host's round-1 wait
  ended 51 ms after it began, when B10 arrived. The switch-in and round 3 asked nothing, and the NPC chose 26 ms after
  the digest. The battle ended normally after 3 rounds. `abort2` shows the same for round 3.

## Partner rewards, Pay Day, trapping and mid-turn switches (28 October 2026)

Protocol 9. User-facing rules: [COOP-ONLINE.md](COOP-ONLINE.md#team-up-trainer-battles). Code: `runtime/hle/coop_battle.c`
(`partner_exp`, `affection_hook`, `result_hook`, `partner_rewards`, `mirror_after_battle_hook`, `prize_hook`),
`runtime/coop_battle_link.c` (`coop_battle_link_mirror_matched`, the B0 hold).

### What the partner gets, and why from its own replay

The guest's engine replays the host's battle command by command, and the per-round digests prove both engines hold the
same battle. So the guest's rewards come from its own engine, computed natively, rather than from data the host would
send. The host sends only what it already sends (its commands, its digests, a finished end with its command counts),
plus one more digest at the battle's result. Recomputing the EXP elsewhere would need participation, faint order,
levels and Let's Go's boosts copied out of the battle, and would miss whatever the game does differently.

- **EXP.** The battle's EXP step `0x70009f2970` (server; called at every knock-out) gives points only to the local
  player's party: `0x70009e0e10(pool, 0x70009da090(main))`, trainer 0. On the guest that is the host's copy, whose
  results are discarded. For the guest's own party (trainer 2) `exp_step_hook` first runs the same calculation
  `0x7000a11a30(server+0x2714, 0x70009d71c0(main), server, party, foe, out[6 x 16 bytes])` for each opponent in the
  step's knock-out list (`server+0x1354`: count `0x7000a66000`, done `0x7000a66080`, id `0x7000a66030`, opponent side
  `0x70009dab70`). The calculation (`0x7000a67c30` -> `0x7000a67dd0` -> formula `0x70009d0df0`) counts participation
  (the foe's faced set, `0x70009e7a50`), gives the rest of the party the share (/24 instead of /12), and applies
  traded x1.5/x1.7, past-evolution-level x1.2 and affection x1.2. Fainted Pokemon get nothing (`0x70009e3f10`).
  Nothing is written to the battle: the points are kept per Pokemon (by encryption constant) with the level they
  raise, and that level is put in the battle Pokemon's `+0x86` only during the next calculation (the formula reads it).
- **Affection.** `0x70009dadc0` returns 0 unless the Pokemon belongs to trainer 0, and the battle copies friendship
  (`0x7000727fd0`) into `+0x2e2` only for the player's Pokemon. During the partner's calculation `affection_hook`
  applies the same rule without the trainer check, on the Pokemon's own friendship. Without it every partner Pokemon
  got 29/14 per knock-out instead of the solo 35/17 (`teamup-rewards1/pair3`). Let's Go's local co-op keeps P2's
  Pokemon in trainer 0 too.
- **The end check.** `result_hook` (`0x70009d3870`, which sets setup `+0x508`, 1 = won) records the result and the
  prize (`main+0x16c`) and sends one more digest (the next round number) on both sides. The guest applies rewards only
  if the replay was not aborted, the host's finished end arrived with matching command counts (`mirror.ended`), that
  last digest matched (`coop_battle_link_mirror_matched`) and the battle was won. Each battle ID is rewarded once (an
  8-entry ring), and only at the result phase of a replay that ran.
- **Applying.** The after-battle process `0x7000319bc0` (state `+0x68`) used to be skipped from state 0 on the guest
  (it applies the battle party, here the host's copy). Now, at state 0, `partner_rewards` adds the EXP to the save
  party's Pokemon with the game's out-of-battle level-up (`0x70008319c0`, the Exp. Candy step: one level per call,
  friendship, HP kept), adds one trainer's prize with the money add `0x70001c6f60`, sets the process's per-slot
  "levelled up" flags `+0xd8+i`, and moves to state 1, the native evolution loop (`0x7000b0c510`, `0x7000728300`, demo
  `0x700083cef0`). The replay's guard against Spirit writes to the saved party (`mirror_spirit_hook`) is lifted first.
  Moves for the new levels are not learned (the in-battle learning runs on the battle client).
- **The wait.** The guest presses through the battle text (`mirror_frame_hook`) and reaches its result phase before
  the host, which reads its own. The host now sends its finished end at its result phase (B6), not on its overworld.
  The guest holds state 0 (the screen is black) for up to 2 minutes, with "Waiting for *host* to finish the battle..."
  (`coop_battle_partner_finishing`, `coop_ui.mm`). `teamup-rewards1/pair2` gave up after a first limit of 15 s, when
  the host's end came 16 s after the guest's result.
- **Prize and Pay Day.** `{1,T,T,T}` has two copies of the trainer, and the prize `0x70009d08d0` sums both opponents'
  `base x level x 4`. It is computed once at the battle's start, into `main+0x16c`. `prize_hook` halves it on both
  clients. Pay Day accumulates separately (`0x70009da520` -> `main+0x170`), and the result adds `+0x16c` and `+0x170`
  (`0x70009d3870`), so Pay Day stays whole for the host. Before, `money_hook` halved the whole total at the add
  (`0x70001c6f60` from `0x7000319e0c`), Pay Day included. The guest gets `+0x16c` only.

### Other fixes in this round

- **Trapping.** `ally_choices` asks the party list's own check `0x7000a5c200(controller, Pokemon, &trapper, &cause)`
  (5 = free; Shed Shell 0x127 frees; `0x7000a5c590`: Arena Trap / Magnet Pull / Shadow Tag, and conditions 0x16, 8,
  0x15 such as Mean Look and Wrap). A trapped guest Pokemon is offered no switch slot in B3, so the host
  (`coop_ally_submit`) and the relay refuse a switch, as for any unoffered slot, and the NPC chooses at the deadline.
- **After an abort.** The guest's slot-0 switch-in (requests 4 and 5) runs the NPC handler `0x7000a58000` instead of
  the human one, so the guest is never shown the host's party list.
- **Digest.** Bits 1-7 of battle Pokemon `+0x92` are now skipped. The halfword at `+0x91` is a bit field: setup
  `0x70009e2490` sets bits 0-8 and battle code uses only those (`0x70009e3ca8` ... `0x70009e792c`). A reserve that a
  U-turn sent in held `0xd0` on the host and `0xd2` on the guest there (`teamup-midturn1/host-uturn`, round 4: the
  replay aborted).
- **Offer countdown.** B0 now carries the host's hold (10 s for a trainer's held text). The guest's offer expires at
  `hold - min(2 x RTT, hold / 4)` and its prompt shows "Expires in N s" (`teamup-rewards2/payday/guest/offer.png`: 8 s
  left 2 s after the offer). `coop_battle_link_offer_deadline` is gone:
  the hold is an argument of `coop_battle_link_offer`.
- **Names.** The prompt's trainer name (and the overlay's move and species names) come from the game's own message
  language, the global `0x7001614b34` (`0x70001b3290`: 0 JPN, 1 JPN_KANJI, 2 English, 3 French, 4 Italian, 5 German,
  6 Spanish, 7 Korean, 8 Simp_Chinese, 9 Trad_Chinese), not always English. The host save in these runs is Spanish:
  its log names Doug "Domin", while the English guest's prompt says "Doug" (`offer.png` above). The names test
  (`battle-names-test.c`, in `run-battle-link-tests.sh`) checks both tables. The overlay font covers Latin-1 and Cyrillic only, so Japanese, Korean and Chinese
  names show as missing glyphs.
- **"Eevee" on the guest's Machamp.** That was the test fixture, not the battle code. The guest saves in
  `tests/saves/coop-mirror-fixtures/guest` and `tests/saves/teamup-switch/guest-*` were made by changing an Eevee's
  and a Pikachu's species, and the game shows a Pokemon's stored name. The solo baseline showed it too ("Eevee used
  Bite"). The fixtures now carry species names (`party-fixtures names`; originals in
  `tests/saves/archives/misnamed-fixtures/`), and both HP boxes read "Machamp" (`teamup-rewards1/pair3`, guest
  `b-8.png`, host `b-6.png`).

### Verification (two clients, copied saves, `tests/scripts/run-teamup-rewards.sh`)

Fixtures in `tests/saves/teamup-rewards/` (`party-fixtures flag`, `setexp`, `setmon`, `prepare-count`): the guest
`guest-unbeaten-charmander.bin` (Doug's flag 2742 cleared; party slot 1 a Charmander, Lv 15 and 1 point short of 16),
`guest-uturn` (lead with U-turn), `guest-growl`, `guest-only-lead-recoil`; the host `host-payday` (Eevee with Pay Day),
`host-uturn`, `host-growl`, `host-only-lead-recoil`. Savediff: `<role>/save-diff-orig-post.txt` (orig = the copied
save before the run, post = saved in game after the battle).

| Run (`tests/runs/`) | Covered | Result |
| --- | --- | --- |
| `teamup-rewards1/solo` (one game, the guest's save alone vs Doug) | Native baseline | Money +36, 3 Poke Balls, flag 2742 set; Machamp +35, the rest +17; Charmander 15 -> 16 and evolved. |
| `teamup-rewards1/pair2` (QA) | First try | EXP worked out on the guest; no rewards: the 15 s wait ended 0.1 s before the host's end. |
| `teamup-rewards1/pair3` (QA) | Rewards before the affection fix | Guest money +36, EXP 29/14 per knock-out, Charmander -> Charmeleon (`guest/b-20.png`), waiting notice (`guest/b-12.png`). |
| `teamup-rewards2/payday` (no QA variables) | Rewards, Pay Day | Host money +536 (36 + Pay Day 500; Pay Day PP 20 -> 19), 3 Poke Balls, flag set. Guest money +36, Machamp +70 (2 x 35), Charmander +37 (17 + 20, x1.2 past its evolution level) -> Charmeleon, the rest +34 (2 x 17); no items, flag 2742 still unset. |
| `teamup-midturn2/host-uturn` (QA) | Request 5 for slot 0 | Host U-turn, host picks Pikachu (`0x83`), the guest replays it; digests matched through the final round; guest rewards as above. |
| `teamup-midturn2/guest-uturn2` (QA) | Request 5 for slot 2 | Guest U-turn, the guest picks in its own list (`guest/switch-1.png`); host `guest sent in its party slot 2`, replay `0x103`; Blastoise, sent in, gets participant EXP (+52). |
| `teamup-midturn1/host-uturn` (QA) | Before the `+0x92` fix | Aborted at round 4 (the reserve's `+0x92`); no rewards. |
| `teamup-rewards2/abort` (QA, `CORRUPT=1`) | Abort, then the host's faint | Guest aborted at round 1; the host's Eevee fainted from recoil and the guest's copy sent in the NPC's pick (`mirror_aborted_npc_switch`, no list in `guest/b-*.png`); guest "partner rewards: none (replay aborted)", save unchanged; host +36, 3 Poke Balls, flag set. |
| `teamup-rewards2/loss` (QA) | Loss | Both single level-2 leads fainted from Double-Edge recoil: result 0 on both; guest "partner rewards: none (battle not won)", save unchanged (money, party). The host whited out to a Pokemon Center as in a solo loss (`host/saved-host.png`; its in-game save was not reached). |
| `teamup-rewards2/disconnect` (QA, host closed mid-battle) | Disconnect | Host closed 21 s into the battle (`KILL_HOST_AT=4`): the relay's end reason 2 aborted the replay (reason 5); the guest finished its copy locally (a local win) and got "partner rewards: none (replay aborted)"; save unchanged. |

The host's savediff equals the previous behaviour in each won run: +36 (+ Pay Day), 3 Poke Balls, flag 2742, PP.
Link and relay tests: `tests/scripts/run-battle-link-tests.sh` (the B0 hold and its countdown margin, the end-of-battle
digest match). Not covered by a run: a trapped guest Pokemon (no Viridian Forest trainer has a trapping move; the check
is the party list's own), a Pokemon learning a move on the guest, and a crash between the rewards and an in-game save
(the rewards are then simply not saved; they can't come twice, since they are only applied at the result phase of a
replay that ran).

## Wild team-ups and the partner's move targets (28 October 2026)

Protocol 10. Rules and decisions: [COOP-TRAINER-ENCOUNTERS.md](COOP-TRAINER-ENCOUNTERS.md#11-wild-team-ups-28-october-2026).
Code: `runtime/hle/coop_battle.c` (`coop_battle_wild_touch`, `wild_event_hook`, `wild_setup_hook`, `wild_add_ally`,
`rule3_ally_*`, `main_init_hook`, `own_hook`, `language_hook`, `move_target_type`, `move_targets`),
`runtime/hle/wildbattle.c` (`wildbattle_replay`, the touch's offer, `LARECOMP_WILDBATTLE_TEST_FILE`),
`runtime/hle/input.c` (P2's controls), `runtime/hle/coop_actor.c` (the replay's records), `runtime/coop_battle_link.c`.

### The battle setup

- The battle event `0x700028be80` keeps its kind at `+0xd0` (1 trainer, `0x700028c3c0` with the 0x30-byte config at
  `+0x88`; 2 static/wild, `0x700028c770` with `{mode, config}` at `+0xb8`). Setup is at event `+0xe0`; its result is
  setup `+0x508`.
- Setup layout (from `0x70009cc510`, `0x70009ccda0`, `0x70009cd7e0`): `+0` kind (0 wild, 1 trainer), `+4` mode (the
  rule: 0 single, 1 double/multi, 2 one vs two, 3 two vs one), `+0x65` multi flags, parties `+0xb0/+0xb8/+0xc0/+0xc8`
  (trainers 0-3), MyStatus `+0x410..+0x428`, trainer data `+0x438..+0x450` (objects of 0xb0 bytes, constructor
  `0x70009ce5d0`, loaded by `0x7000b0ec00`), trainer info `+0x1a0/+0x270/+0x340` (`0x7000b0f3d0`); a trainer's
  party is loaded by `0x7000b0ed20`.
- Position to trainer: `0x70009e1bf0(rule, flags, position)`, tables at `0x7000f46b3c` (rule 0 `{0,1,-,-}`),
  `0x7000f46b6c` (rule 2), `0x7000f46b7c` (rule 3 `{0,1,0,-}`) and, for rule 1, `0x70015c7590[flags-1]`
  (`{0,1,2,3}` multi, `{0,1,0,3}`) or `0x7000f46b4c` (`{0,1,0,1}`). Positions per trainer: `0x70009e1a10`. A trainer
  exists when a position maps to it (`0x7000a54c30`). Non-local trainers with an NPC controller: `0x7000a54ce0`
  (rule 3: trainer 1 only; table `0x7000f4b1d8`).
- The main module's init is chosen in `0x70009d2ce0` from the setup: mode 1 with flags 0 `0x70009d4750`, mode 1 with
  flags `0x70009d4970` (the step table `0x70015c73f8`, by presence), every other mode `0x70009d4490`, whose steps 2-5
  build the server's (`main +0x140`) and client's (`+0x148`) battle parties of trainers 0 and 1 only
  (`0x70009d5d50(main, pool, trainer, client)`; sources `main +0xe0 / +0x100`).
- A static/wild battle in rule 3 with an ally therefore needs: position 2 -> trainer 2, one position each for
  trainers 0 and 2, an NPC controller for trainer 2 and trainer 2's two parties. The UI then builds the ally's
  model and Pokemon as for a trainer multi (`0x7000a6e670` reads the position's Pokemon from the client pool).

### Other findings

- 2 vs 1 input: the menu of position 2 reads P2's controller (a sideways right Joy-Con: its stick is the right
  stick). The partner's lent menu there got no input until `input.c` served P1's controls as P2's while it is open.
- EXP formula input (`0x7000a67dd0`): `+0x8` the player's language (`0x70001c9d40`, MyStatus `+0x8d`), `+0x1c` the
  Pokemon's, `+0x30` own Pokemon (`0x70001c9d90`: MyStatus ID, gender and OT name); `0x70009d0df0` applies x1.5
  (`0x1800`) or x1.7 (`0x1b33`, another language) when not own.
- Level-up in battle: `0x70009e70d0` (EXP set `0x70007268a0`, stats `0x7000725210` / max HP `0x7000725ef0`), run
  for both pools' copies of the Pokemon by the client's command playback (`0x7000a61830`, request 7).
- Requests at the end of a won battle: 7 (commands), `0xd` (`0x7000a590a0`), `0x12` (`0x7000a59530`); the result
  `0x70009d3870` comes after them. Request `0xe` (`0x7000a59170`) shows the end messages of a lost battle.

### The partner's move targets

Before, `ally_choices` offered only target type 0, so the partner's menu took back Growl, Swords Dance and the like.
Now every move is offered with the positions its type may name (`move_targets`): type 0 (any adjacent) the living
foes and the partner's ally, 1 (user or ally) itself and its ally, 2 (ally) the ally, 3 (one foe) the foes, 7
(itself) its own position; every other type (all adjacent, all foes, a side, the field, a random foe) names no one,
and any position or 4 (what the NPC sends, e.g. `0x2d41` for Growl) is accepted. The host builds the command from the
move and the position its native menu sent (`0x7000a50a90`); the relay checks the same masks. The NPC's own pick is
still pointed back at a foe only for types 0 and 3. The overlay lists the ally and itself too.

### Verification

See [COOP-TRAINER-ENCOUNTERS.md section 11](COOP-TRAINER-ENCOUNTERS.md#11-wild-team-ups-28-october-2026). Offline
experiments (one game, `run-teamup-wild.sh ally`, QA ally `LARECOMP_COOP_WILD_TEST_ALLY`): `teamup-wild1/e1` (rule 1
crash), `e7` (rule 3 stuck: no NPC controller), `e12` (first working 2 vs 1 with an ally), `catch-nav1` (the Bag and
a ball in the 2 vs 1), `learn3` / `learn4` / `learn5` (a level-up with the native party, a shared clone, a clone per
pool).
