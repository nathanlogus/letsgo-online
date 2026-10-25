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

`runtime/coop_party.{h,c}` defines a pointer-free party snapshot: a count of 1?6 and 260 bytes per native serialized PB7. Validation checks the encrypted core checksum, bad-egg flag and calculated level/HP/stat bounds. A digest supports comparisons; it is not authentication or a complete battle-state digest. No party packets are sent by protocol 5 yet.

`runtime/hle/coop_battle.c` runs opt-in, local QA on Eevee 1.0.0. The native party serializer (`0x7000729650`) and importer (`0x7000729660`) round-trip six Pok?mon from a different copied save. Temporary objects use the native constructor/destructor, never the saved-party insertion or capture-registration path. Native serialization of the local party is compared immediately before/after import and remained identical. The exported bytes also decode correctly in PKHeX.

Ordinary trainer process factory: `0x700028bb60`, receiving a 0x30-byte configuration. Its trainer fields are **ally, opponent 1, opponent 2**, in that order. A simple solo trainer configuration is `{0, 0, trainer_id, 0, ...}`. Putting the trainer in the ally field leaves the enemy party empty. Trainer 26 loads a level-5 Rattata; a copied-save automated run opened Fight, selected a move and returned to the overworld at game frame 3875. A screenshot verified the move menu. The initial return capture exposed a black fade: the field callback resumed, but the skipped story script had not performed FadeIn. The QA helper now schedules that native FadeIn 30 frames after return. Final solo and foreign-lead runs visibly returned to the overworld in frame-4200 captures; their field callbacks resumed at frames 3475 and 3875 respectively. This test does not run a trainer's overworld story script.

The top-level choice observer composes with the existing hook through `rt_register_fnhook_chain`. It copies the choice while the UI object exists; the object may be destroyed when its submenu opens. Do not retain its pointer across frames. This observer still does **not** capture a complete move/target decision.

QA controls (all output directories must already exist):

- `LARECOMP_COOP_PARTY_TEST_DIR`: export `party.bin`, `import.txt` and local battle journal. Fixture format is `LGPTY001`, one count byte, then count ? 260 serialized bytes.
- `LARECOMP_COOP_PARTY_TEST_PEER`: a local fixture from a second copied save.
- `LARECOMP_COOP_TRAINER_TEST_AT`: start the trainer factory on an overworld update at/after this game frame; requires an explicit save directory and party-test directory.
- `LARECOMP_COOP_TRAINER_TEST_ID`: ordinary trainer ID, default 26.
- `LARECOMP_COOP_TRAINER_TEST_GUEST`: experimental multi-trainer setup with the foreign lead imported into the battle-owned ally party. The multi-trainer configuration fills both opposing slots, which the native mode requires. The foreign level-50 Machamp rendered alongside the host?s level-100 Eevee. This investigates ownership and presentation only; the native NPC ally still controls its decisions.

Fixtures, copied saves, test binaries, scripts, logs and captures stay under ignored `tests/`. Checksum/count/level/HP rejection tests and hook-composition/reinstallation tests pass. Real two-client battle authority, completed command ownership, RNG/state replication, disconnect recovery and reward rules remain pending. The normal social tag still keeps battles individual.

Final copied-save checks: the host retained all six original species, encryption constants, personality IDs, trainer IDs and forms after both battles. Checksums remained valid. The host lead consumed one PP normally; the foreign Machamp was not inserted into the host party. The two original manual-test save SHA-256 hashes remained unchanged. These checks establish temporary ownership for this local prototype, not network command synchronization or a final shared reward policy.
