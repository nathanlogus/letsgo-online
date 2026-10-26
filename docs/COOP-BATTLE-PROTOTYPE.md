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
