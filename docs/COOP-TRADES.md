# Online trades

Status (28 October 2026): implemented and covered by unit and relay tests; **not yet run in a live game**. The live
checks are under "Trades" in `tests/coop-final-checklist.md`. Eevee 1.0.0 only: the Pikachu build has no addresses for
the native steps yet.

Two players in the same room trade one party Pokémon each. Only those two Pokémon change, never other save data, and
whole saves are never exchanged. Social mode stays independent. The user guide is in
[COOP-ONLINE.md](COOP-ONLINE.md#trades).

## Files

| File | Role |
| --- | --- |
| `runtime/coop_trade.c/.h` | The state machine, the `W` messages, PB7 decoding and checks, the record `<save>.trade` and its recovery. It doesn't depend on the game, so the unit tests use it directly. |
| `runtime/hle/coop_trade_apply.c` | The game-thread side: the party list for the offer, the native apply, and the save-image hook. |
| `runtime/ui/coop_trade_ui.mm` | The overlay: the request pop-up, the trade window and its notices. |
| `runtime/services/fs.c` | `coop_trade_recover` runs when the save opens; `coop_trade_after_replace` runs after each whole-save write. |
| `server/coop_relay.py` | `trade_action`, `trade_decide`: the relay is the commit point. |
| Entry points | `runtime/hle/coop_menu.c` (Communicate → Players → trainer → Inspect trainer → Trade Pokémon), `runtime/ui/coop_ui.mm` (Friends → Trade Pokemon), `runtime/coop_net.c` (the `W` channel). |

## Flow

1. **Request.** One player sends a request. The relay forwards it only if both players are tagged up, or both are on the overworld within 30 m in the same area, and no battle is running. Otherwise it answers abort, reason 6. The other player gets the invitation pop-up (Trade / Decline, 45 s).
2. **Offers.** Each side picks one party Pokémon. Every pick is a new revision and is sent in full: 260 bytes of encrypted PB7, in two chunks. The other side validates it before showing it.
3. **Inspection.** The window shows both offers decoded from the PB7: nickname, species, level, gender, shiny, OT name and ID, held item, moves. It is an ImGui panel. The game's summary screen was not used, because calling it from the overlay has not been verified.
4. **Confirmation.** A confirmation names both revisions (`own rev, their rev`). It counts only while both offers are unchanged. Any new offer, on either side, clears both confirmations, in both clients and in the relay.
5. **Commit.** See below.

### Refusals

- **The partner Pikachu or Eevee** (species 25 form 8, 133 form 1; the game's `0x70001760d0` checks the same) can't be offered. The native apply checks it again.
- **The last Pokémon that can still battle** (HP > 0) can't be offered. The game's `PokePartyRemoveMember` applies the same rule.
- **The peer's offer** must pass `coop_trade_offer_valid`: the checksum and field bounds of `coop_party_pokemon_valid` (also used by battle imports), a Let's Go species (1–151, 808, 809), not an egg, and not the partner. A failure cancels the trade (result 7). The bytes reach the game only through the native importer, after this check.
- **Clone identities.** A Pokémon is identified by its encryption constant and PID. If the two offers share an identity, or the save holds a copy of either one, the apply refuses (quarantine) rather than guessing.

## Commit protocol and recovery

The relay is the coordinator, in a two-phase commit. Each client keeps a durable record `<save>.trade`, written to `.tmp` and then renamed over the old one with `plat_replace_file`.

| State | On disk | Meaning |
| --- | --- | --- |
| offered / confirmed (OPEN) | nothing | Negotiation only. |
| **prepared** ("sent") | `state prepared`, both PB7s, the peer's identity | Written before the client sends W4 "prepared" with the digest of both offers. |
| **commit** ("received") | `state commit` | The relay received both "prepared" messages with a matching digest, decided commit and told both sides (W5). |
| applied | `state commit` | The trade is in the running game, waiting for a save. |
| **committed** | `committed <id>` (history) | A whole-save write after the apply succeeded. |
| aborted | `aborted <id>` (history) | The relay decided abort, or the peer resolution below did. |

The relay commits only when both sides have sent "prepared" for the exact offers they both confirmed. The relay computes the digest itself from the bytes it forwarded. Before that point, anything aborts the trade:

| W5 abort reason | Cause |
| --- | --- |
| 1 | Cancel or decline |
| 2 | Disconnect or removal |
| 3 | The request expired |
| 4 | Both sides confirmed but did not both send "prepared" within 30 s |
| 5 | Digest mismatch, or "prepared" received after an offer change |

Every decision is kept for 7 days. With `COOP_RELAY_TRADE_LOG=<file>` it is also appended to a file and fsynced, so it survives a relay restart.

**Applying it.** The game thread applies the trade at the next overworld update. It follows the game's own Link Trade: storage is one list of 1000 slots, and the party is a list of indices into it.

1. The received PB7 is imported into a native Pokémon on the game's heap, owned through its shared reference as in `0x7000838900`.
2. The receiver becomes its handling trainer (`0x7000b0c3b0`). The OT is never rewritten.
3. The Pokédex follows the trade demo: `0x7000b081d0` with type 4, then `0x70001cffe0` when that call leaves registration to its own screen.
4. A trade evolution is checked with `0x70007283e0` and applied with `0x7000728450`, then registered again. This is the scene-less trade path of `0x7000838900`, so there is no evolution cutscene.
5. The new Pokémon is copied over the sent Pokémon's storage slot (`0x70001c0fd0`). The party order stays the same and nothing else changes.

The trade becomes **committed** when a save image built after the apply (the `0x70001cddf0` hook) has been written successfully.

**What each side does on restart or reconnect:**

| Situation | Action |
| --- | --- |
| No record, or only history lines | Nothing. |
| `prepared` (in doubt) | It asks the relay (W7) once per connection. Commit: apply. Abort: drop the record. If the relay has no decision (it restarted without a trade log), it asks the peer (W8). The peer answers committed (2), aborted (3), in doubt (1) or unknown (0). Only "committed" means commit; any other answer means abort, because no one could have applied the trade without the relay's commit. Without an answer it stays in doubt: the Pokémon are untouched and no new trade can start. |
| `commit` | The save that was loaded is checked. Sent Pokémon present and received absent: apply again. Sent absent and received present: the save already has it, so it is committed. Anything else: the record is moved to `<save>.trade-quarantine-<id>` and nothing changes. |
| A malformed record | Moved to quarantine. Nothing is applied. |

**Guarantees.**

- Neither side applies a trade without the relay's commit.
- The relay commits only after both records are on disk.
- A side with a commit record keeps re-applying until a save carries the trade. Applying is idempotent: it checks the identities in storage first.
- So a crash or a disconnect at any point leaves at most a delay: the trade is either done on both sides or on neither.

**Remaining risk windows.**

- **The commit record can't be written** (disk error) and the game then ends before a save. That side loses the trade while the other keeps it. This is logged.
- **The relay restarts without `COOP_RELAY_TRADE_LOG`** after sending its commit, the commit reaches one side, and the other side never receives it. The second side then learns the outcome from the peer's own state. Only a peer that lost its record as well could leave it wrong.
- **The player never saves after an applied trade.** The record keeps it pending, and it is re-applied at the next boot.
- **An in-place reload of the save inside one process** after an apply. The in-image flag assumes the running game's state is still the one the trade went into.

## Wire changes (protocol 11)

New opcode `W`. Each body is: subtype (u8), trade ID (u64 LE, random, chosen by the requester), then the fields below.

| Subtype | Body bytes | Direction | Contents after the ID |
| --- | ---: | --- | --- |
| 0 request | 9 | client → peer | — |
| 1 accept | 9 | client → peer | — |
| 2 offer | 144 | client → peer | revision u32, chunk u8 (0/1), 130 PB7 bytes |
| 3 confirm | 17 | client → peer | sender's offer revision u32, the other's revision u32 |
| 4 prepared | 17 | client → relay | digest u64 = FNV-1a64(offer A) + FNV-1a64(offer B), mod 2^64 |
| 5 decision | 11 | relay → client | decision u8 (0 unknown, 1 commit, 2 abort), reason u8 |
| 6 cancel | 9 | client → relay / peer | — |
| 7 query | 9 | client → relay | — (answered with 5) |
| 8 status | 11 | client → peer | state u8 (0 unknown, 1 in doubt, 2 committed, 3 aborted), ask u8 |

An older relay answers `W` with "unsupported message" and closes the connection. A client and relay without `W` therefore must not be mixed, which is why `W` came with protocol 11. No existing message changes.

## Tests

`tests/scripts/run-trade-tests.sh` builds and runs both tests. The log is `tests/logs/trade-tests.log`.

- **`tests/tools/trade-test.c`** covers:
  - PB7 decoding, checked against PKHeX for the fixture;
  - the rejections at the trust boundary;
  - the partner and last-Pokémon refusals;
  - confirmations cleared by an offer change on either side, and stale confirmations ignored;
  - interruption in every state: request, invitation, open, prepared (lost link, and a crash with the relay committing or aborting), commit (a crash before the apply and before the save), applied (a failed save write, and an image built before the apply);
  - peer resolution;
  - quarantine.
- **`tests/tools/trade-relay-test.py`** covers:
  - the tag/proximity and battle gate;
  - accepting one's own request;
  - offer revisions and locking;
  - confirmations for the current revisions only;
  - the commit point;
  - each abort reason;
  - queries;
  - status forwarding;
  - decisions reloaded from the trade log.

The fixtures for the live checks are in `tests/saves/trades/` (`party-fixtures reroll/setexp/setmon/uncatch`; `tradeinfo` prints identities and trainers).

Static analysis: `tests/cache/ghidra/out/trade-*.c`.
