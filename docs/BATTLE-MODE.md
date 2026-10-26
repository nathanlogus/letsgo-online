# Game mode: battling wild Pokémon

_Idea (15/10/2026): when touching a wild Pokémon, instead of going straight to the catch (Pokémon GO style), enter
a classic battle - and catch it afterwards, as the game already does with the legendaries and Snorlax. As far as anyone knows, nobody
has done this in a Let's Go mod ([2026 discussion](https://reddit.sentinel-team.org/posts/1rr2xif/snapshots/2026-03-13T05%3A19%3A13.96841Z))._

## Current state (25/10/2026)

Eevee 1.0.0 uses classic combat by default: no timer, XP on defeat and
catching during the battle through the native Bag. Balls and medicines use the
real inventory; the catch records trainer, location, date and ball and adds the Pokémon to the
party or to storage. The result does not open another catch screen.
Story battles keep their rules. Pikachu still uses a battle followed
by the GO catch. The switch is in Eevee's native Options and in the launcher.

The in-combat catch shows the trainer throwing the model of the chosen ball
toward the wild Pokémon, with impact, drop and wobble before the result.
An escape returns the wild model to the scene. The assets come from the game itself;
the port reconnects the ball's creation and adapts the commands of the native sequence.

The rest of this document keeps the history of the investigation, including
limitations and earlier attempts. Current implementation: `runtime/hle/wildbattle.c`
and `runtime/hle/wildbattle_classic.c`; diagnostics in [DEBUGGING.md](DEBUGGING.md).

## Step 1: map of the two paths ✅

Method: `LARECOMP_FNPROF` (runtime/fnprof.c: game functions executed within a window of frames, now with the order of
first entry and the caller) in runs with no encounter, with a wild encounter and with a trainer battle (save on
Route 1, next to Youngster Ronny); `tools/fncompare.py` separates what is common and what belongs to each event, and
`LARECOMP_FNPROF_BT=<addr>` + `LARECOMP_FNPROF_BT_MAX=<n>` show the call chain.

Map update (every frame): `main+13c8f0` → `13d2f0` → `315390` → `2f78d0` → **`2f8bb0`**, which calls:
- **`2fda00`** (wild Pokémon on the map). On touch: → **`305ad0`** → **`28ef70`**`(x0 = encounter object,
  x1 = global manager [*0x70015faab8]+0x68)`, which allocates the catch scene (0x3B0 bytes) and pushes it:
  `31c030` → `139e80` (push process/scene).
- **`2facf0`** / `2fa860` (trainers' line of sight). On seeing the player: `17a7f0` → `17aa70` → `17b380` → `139e80`
  (event: "!" and dialogue).

Trainer battle, after the dialogue: script interpreter `17b6c0` → `19ca10` → `19b7f0` → command
`674ed0` → `672484` → `66fc20` → `17e150` → **`28bb60`**`(x0 = &object, x1, x2 = &parameters)`, which copies a
battle configuration of **0x30 bytes** into the battle object (+0x88) and switches scene (`28bc30` → `28e480` →
`139e80`); inside the battle, `28be80` → `28ccb0` → `28b150` → `31a470`.

Conclusion: catch (`28ef70`) and battle (`28bb60`/`28e480`) are neighbouring methods of the same encounter manager
(`main+28xxxx`). The detour would be in `305ad0`: build a wild battle configuration for the touched Pokémon and
call the battle start instead of the catch; after the battle, start the catch (the legendaries' flow).

## Step 2: hooks and first data ✅

- **Function hooks** (runtime/fnhook.c, `FN_ENTER` in runtime/cpu.h, DLL ABI 3): the runtime can handle or
  wrap any game function without regenerating the DLL. `LARECOMP_HOOK_DUMP` logs arguments and memory.
- **Youngster Ronny's battle** (`28bb60`, called only by the script command `17e150`):
  - `x2` = 0x30-byte parameters: `{0, 0, 0x1C1, 0, 0x200, ...}`; the script command receives the same 0x1C1 and
    0x200 - **0x1C1 (449) must be the trainer**, 0x200 flags;
  - `*x0` = large reference-counted object (vtable `0x7001537770`): the event's battle request.
- **Other paths** (direct references in `main`, `tools/xrefs.py`):
  - `28bc30` (actually starts the battle) has two callers: `28bb60` (script) and **`28bda0`**, called by
    `289c4c` - a virtual method that builds the parameters with a **mode 0 or 3** depending on the flag `[obj+0xA6]`;
  - the catch `28ef70` has four callers: the wild encounter (`305ac4`), `28ea90`, `854a48` and **`31a0e0`**,
    called by the virtual method `319e20`, next to the battle sequence (`31a470`) - candidate for the legendaries'
    "battle and then catch".

## Step 3 (partial, 17/10/2026): what the static research and the dumps showed
**Callers checked** (search for a direct `bl` to the exact address; `xrefs.py calls` gets the function start wrong
when there is padding before it):
- `28ef70` (catch): `28ead8`, `305b34` (wild encounter), `31a140` (fn `31a0d8`), `854afc`. Matches step 2.
- `28bc30` (start battle): only `28bb80` (script) and `28bdbc`; `28bb60` only `17e218`; `28bda0` only `289ce0`.

**`289c4c` and `319e20` are not standalone methods**: they are jump-table cases of state machines (state in
`[obj+0x68]`) of two classes with their own vtable. Class A: vtable at `image+0x1546ea8` (GOT slot `0x15ff050`),
constructors starting at `289ac0`, factory `17f098` (allocates `0xA8` bytes). Class B: vtable at `image+0x154cb70` (GOT
`0x15ff8c0`), constructor `318f50`, factory `291470` (allocates `0x100` bytes). The factories follow the same pattern (allocate,
construct, push with `13a750(manager, &object)`), which suggests a large family of "processes".
- Class A, case `289c4c`: builds `{u32 mode, config pointer}` with mode `3` if `[obj+0xA6] != 0`, otherwise `0`, config =
  `[obj+0x90]`, and calls `28bda0`, which starts the battle (`28bc30`) and writes mode to `battle+0xb8` and config to `+0xc0`.
  Then it calls `28dd20`/`28dd40` with the flags `[obj+0xa0..0xa2]`.
- Class B, case `319e20`: calls `31a0e0` (which calls the catch `28ef70`) and returns state 4.
- It is still **not confirmed at runtime** that A + B form the legendaries' "battle and then catch" flow, and I don't know
  who instantiates the factories (they are virtual methods, with no direct `bl`).

**Dump of a real wild encounter** (`LARECOMP_HOOK_DUMP`, frame 1545 on Route 1): `305ad0` receives `x0` = the
Pokémon's object (vtable `0x70159c848`, fields `+8 = 2`, `+0x28 = 4`), `x1` = manager; `28ef70` receives `x0` = object with
vtable `0x7001537770` (**the same class as the "battle request" of `28bb60`**) and `x1` = the global manager's allocator.
What identifies the touched Pokémon's species/level has not been read yet.

**Test pitfalls** (so as not to repeat them):
- `LARECOMP_HOOK_DUMP` requires the **full guest** address (`700028ef70`), not the offset in `main` (`28ef70` triggers
  nothing, with no error).
- `fps60.c` registers hooks on `28ef70` and `28bb60` when `LARECOMP_SHOT_REL` is on, and `teste60.sh` always
  turns it on: the last registered hook wins, so the dump is cancelled. Use `tools/repro.sh` directly with the script.
- The encounter roll changes the outcome of the same script: the "catch" one sometimes lands in a trainer battle and the
  "battle" one sometimes in a catch. Check the final image or the log before concluding.
- Worktree: `extracted/`, `generated/` and `.venv/` go in as junctions; copy `build/lasgame.dll` **and
  `build/lasgame.dll.id`** (without the `.id` the runtime rejects the DLL: "built for another version") and build only the runtime
  (`make -j20 build/lasrecomp.exe`).

## Step 3b (17/10/2026): what the community already has, and the table of "static" battles
**Community** (research on 17/10/2026): [pkNX](https://github.com/kwsch/pkNX) edits Let's Go through **romfs
data** (paths in `pkNX.Game/File/GameFileMapping.cs`, `FilesGG`): static encounters
(`bin/script_event_data/event_encount.bin`), gifts (`add_poke.bin`), trades (`field_trade_data.bin`), wild
tables (`bin/field/param/encount/encount_data_{p,e}.bin`, FlatBuffers `wild.fbs`), trainers
(`bin/trainer/trainer_{data,poke,type}`). Shiny rate, TMs and type chart live in `main`, and pkNX finds them by
byte pattern and changes them (`EditorGG.cs`). The data mod [Peerless Pikachu & Exquisite Eevee PLUS](https://www.pokecommunity.com/threads/pok%C3%A9mon-lets-go-peerless-pikachu-exquisite-eevee-plus.541476/)
(made with pkNX) changes wild Pokémon, statics and trainers, but in the [release discussion](https://reddit.sentinel-team.org/posts/1rr2xif/snapshots/2026-03-13T05%3A19%3A13.96841Z)
a comment says nobody has found how to add ordinary wild battles, and several people ask for it. In other words: **editing the
statics table is known; turning an ordinary wild Pokémon into a battle has not been done by anyone** (and nothing in the sources says it is
impossible).

**The table of static battles, in our romfs** (`extracted/romfs/bin/script_event_data/event_encount.bin`, 10
records of 0x40 bytes; pkNX's `EncounterStatic7b` layout: `+0` u64 hash, `+8` u16 species, `+0xA` form, `+0xB`
level, `+0xC` shiny, `+0xD` gender, `+0xE` nature (25 = random), `+0xF` ability, `+0x20` 4 u16 moves, `+0x2C`
IVs, `+0x32` EVs, `+0x38` stat boosts, `+0x10..0x1F` text pointers):
| # | Pokémon | Level |
|---|---|---|
| 0 / 1 | Pikachu (form 8) / Eevee (form 1) starters | 5 |
| 2 / 3 / 4 | Articuno / Zapdos / Moltres | 50 (+1 to all stats) |
| 5 | Mewtwo | 70 (+1 to all) |
| 6 / 7 | Snorlax (two, +1 Atk / +1 Def) | 34 |
| 8 / 9 | Voltorb / Electrode | 42 |
Each battle is found by a **u64 hash** that comes from the event script. This is the "wild battle request" we
were looking for: a wild Pokémon's species, level, gender, nature, IVs and moves fit in a record like this.

Clues in `main` (string table near `image+0xf04100`): scenario names `sinario_densetu_mae` and
`sinario_densetu_hokaku` (densetsu = legend, hokaku = capture) next to the three file paths above. The code does not
reference the names via `adrp`+`add` nor by direct pointer (the search found nothing), so the loader uses indices.

**Revised prototype plan**: instead of building the battle request from scratch, reuse the script command of the
static battles with a **synthetic record** (copy Snorlax's and fill in the species, form, level, gender,
nature, IVs and shiny of the touched Pokémon; the moves come from the learnset by level). Still to find: the table's
loader (the table stays in memory after being read: its address can be found by scanning the heap for Snorlax's hash
`343e35139d576e27`) and the script command that receives the hash. Another path already proven by the community: cheats and
"teleport" and "make legendary appear" mods change position and flags (`bin/flagwork`); since guest memory is
host memory, the runtime can do the same to **reach a native Snorlax/legendary battle** and observe the
real flow, without an advanced save.

## Step 3c (17/10/2026): who reads the table by hash = class A, the static battles' process
(Correction: the "readers" of `event_encount.bin` show up via `adrp`+`add` at the start of **string** `+0x156` of the string
table at `image+0xf04000`, and not at the point where the file name starts inside it, which was my mistake in the search.)

The table **is not in memory during the field**: only `field_trade_data.bin` is read at boot (`LARECOMP_LOG_FILES=1`);
`event_encount.bin` and `add_poke.bin` are read **on demand**, when an event asks for them. Guest memory scan
for Snorlax's hash at frame 1400 on Route 1: 0 hits (`LARECOMP_SCAN`, see below).

The code, in `main`:
- **Table loader** (class with a `0x140`-byte object, ctor `2ba800`): `2ba920(this)` requests the file
  `bin/script_event_data/event_encount.bin` (string `+0x156`, via `70001d5150` and `70000a63d0`); `2baae0` says whether it has
  loaded (`1` when `[this+0x130]` or the file is ready); `2bab70` walks `size >> 6` records of 0x40 bytes,
  converts each into a 0x59-byte structure (hash, species, form, level, shiny, gender, nature, ability,
  moves, IVs, EVs, boosts) and inserts it into an `unordered_map` keyed by hash (`2baea0`). **`2bad00(table, &key)`** is the
  lookup: it returns `node + 0x18` (the pointer to the structure). Used only by the classes of states `289b40` (A), `28e99c`
  (`28ea90`, which calls the catch `28ef70`) and `28f454`.
- **Class A** (vtable `image+0x1546ea8`, `0xA8`-byte object, factory `17f098`) is the static battle's process:
  `[+0x68]` state; `[+0x88]` table object; `[+0x90]` battle configuration (`0x2b8` bytes); **`[+0x98]` the battle's
  (u64) hash**; `[+0xa0..0xa2]` and `[+0xa6]` flags. States: 0 (`289b7c`) creates the table object and requests the file;
  1 (`289e18`) waits (`2baae0`); 2 (`289e40`) allocates the configuration, **looks up `[+0x98]`** (`2bad00`) and fills the
  configuration with `2f60c0(config, record)` (it writes `[config+0x298] = 1` and copies species, level etc.); 3 (`289c4c`)
  starts the battle (`28bda0`, mode 3 if `[+0xa6] != 0`, otherwise 0) and then `28dd20`/`28dd40`; 4 not read.
- The factory `17f098` is called from a virtual method (the event's script command), which receives the process
  manager and the allocator and then fills in the hash and the flags on the returned object (to be confirmed).

**Consequence for the prototype** (two options, the second is the cleanest):
1. Create class A through the factory `17f098` and write Snorlax's hash to `[obj+0x98]` (native test battle).
2. **Hook on `2bad00`**: if the key is one of our hashes, return a synthetic record (0x59-byte structure,
   same layout as the parser) with the species, level, gender, nature and IVs of the touched Pokémon; the rest of the flow
   (configuration, battle, catch afterwards) is the game's.

**New tool**: `LARECOMP_SCAN="<bytes hex>[@<frame>]"` (+ `LARECOMP_SCAN_BACK=<n registros>`) scans the guest's mapped
memory (Windows, `runtime/hle/memscan.c`) and logs each hit, the 0x40 bytes and what points to the base.

> **State on 17/10/2026:** works (experimental). Summary, symptoms and fixes in [HISTORY.md](HISTORY.md), section
> "Wild battles mode". The steps below are the research diary.

## Step 4 (17/10/2026): the prototype works; the game's script natives
**Prototype** (`runtime/hle/wildbattle.c`, `LARECOMP_WILDBATTLE=1`): the hook on `305ad0` (the trigger for touching a
wild Pokémon, which calls the catch and then configures the returned handle, which is why it is replaced entirely) creates the
class A process through the factory `17f0a0`, the hook on the constructor `289ac0` writes our hash to `[+0x98]` and the hook on `2bad00`
answers that hash with a copy of Snorlax's real record (template) with species, level and moves swapped. Played by
a human on Route 1: touching a Pokémon opens the battle (level 3 test Rattata) and, at the end, the catch of the battle's
species. Open problem: black screen when returning to the world (the world is drawn, the final passes come out black: a fade).

**The names of the script natives are in `main`** (strings at `image+0xeff9xx..0xf01dxx`, pointer to the function in the next
slot of the natives table): `CallTrainerBattleCore` `17e150`, **`CallWildBattle` `17e440`**, `GetTrainerBattleResult_`
`17e700`, `GetWildBattleResult_` `17e7c0`, `CallBattleLose_` `17e880`, `SaveEventEncountPokemonParam` `17ed00`,
`ClearEventEncountPokemonParam` `17ee70`, `DisappearEncountSymbols` `185cc0`, `EncountPokeSetVisible` `189fe0`,
`FadeIn_` `1a7680`, `FadeOut_` `1a7860`, `FadeWait_` `1a7aa0`, `FadeCheck_` `1a7ad0`. The scripts are AMX (Pawn) in
`bin/archive/script_amx.gfpak` (GFLXPACK: 705 files, LZ4; format in `pkNX.Containers/Misc/GFPack.cs`) and **do not keep
names**, only hashes.
- **`CallWildBattle(amx, params)`**: params = `{[1] hash u64, [2] flags u32}`. Creates class A (factory `17f0a0`) and writes
  `[+0x98] = hash`, `[+0xa0] = [+0xa1] = 0`, `[+0xa2] = 1`, `[+0xa3] = !(flags>>3 & 1)`, `[+0xa4] = !(flags>>1 & 1)`,
  `[+0xa5] = !(flags>>2 & 1)`, `[+0xa6] = 1` only in a special case (otherwise 0), and suspends the script (`672e50(amx, 0xc)`) until the
  process ends. The **constructor**'s defaults (`a0 = a1 = 1`, `a2 = 0`) are the opposite: the first prototype left them like that.
- `FadeIn_(amx, params)`: `{[1] frames, [2] FNV-1a 64 hash of the type name}` -> index 1..11 -> engine function
  `11ece0(manager, 0, type, frames)`; `FadeOut_` ends in `11ee50(manager, 0, type, ...)`; fade manager
  = `[[*GOT 0x70015facf8]]`. Type names (strings at `image+0xefd685..0xefdb2e`): `fade_front_{fade,wipe,r,trainer,pika,
  bui,poke_get,legend,legend_get,loading}_{in,out,keep}`.
- The script does FadeOut -> CallWildBattle -> (wait) -> GetWildBattleResult_ -> FadeIn..., which the prototype does not do.

## Step 5 (17/10/2026): split XP, what is known and what did not work
Goal: half the XP on "defeat" (in the legendaries' flow the Pokémon does not faint: the battle ends and the catch begins) and
the other half on the catch. **Not implemented.** What the research turned up:
- The catch reward screen shows the XP: **level 5 Metapod = 20**, **level 4 Pidgey = 11** ("Bonus Multiplier 1.0,
  Successful Catch", "Exp. Points"); then comes the party screen ("Oddish grew to Lv. 9!"). A trainer battle shows
  "Your party Pokémon in waiting also got Exp. Points!" (battle XP is granted through another path).
- I did not find the function that adds the XP. Attempts: `PokePartyAddParam` (`197bb0`, script native) delegates to a per-member function
  (`19aab0`) and to the Pokémon object (`[obj+0x98]` = CoreParam; `727fe0` is friendship, `725100`/`725800` EVs/IVs by index);
  the level 100 XP constants do not appear in the code (the curves come from `bin/pokelib/grow_table`); comparing function
  profiles (flee vs. catch) and searching for PB7 in the heap gave ambiguity/false positives; heap snapshots
  (`LARECOMP_SNAP`) work but the heap has too many counters to isolate the delta.
- Paths that would still be worth it: watch (`LARECOMP_WATCH`) the XP of a **known** member with the address found by a
  catch played by hand in the same boot; or find the function that writes the "Exp. Points" screen (the value 20/11 goes through a
  message `SetNumber`); or add the XP ourselves after finding the field.

## Step 6 (22/10/2026): catching in the middle of the battle, with HP and status counting
Goal (user request): a new, native option in the battle menu ("Catch"), one ball at a time, higher chance with
the wild Pokémon weak or with a status, fainted = chance lost. Everything below was seen at runtime (save `build/save-edit`,
Route 1, Bulbasaur walking along), except where it says "static".

**The battle engine no longer has the classic throw.** The ball sequences in `bin/battle/waza/sequence` (`ee410`,
`ee412`, `ee420`, `ee422`, `ee430`..`ee460`) are all for sending the Pokémon into the fight (`to_ee400_ball`,
`Play_SE_battle_sys_018_ball_throw`); there is no shaking and no catch. The catch is always the GO-style scene.

**From the end of the battle to the catch** (states of the battle event `28be80`, word `[+0x68]`; `[+0x720]` the battle):
- 0 prepares, 1 waits for the fade, 2/3 create the battle, **4 battle running**, 5 end: creates the post-battle process
  (class B, update `319bc0`) with `[B+0x88]` = the battle and `[B+0xb8]` = type (`[event+0xd0]`, 2 here);
  6/7/8/12 wrap up and the class A process ends.
- The **result** is at `[[event+0x720]+0x68]+0x508`: **1 victory, 3 flee**. State 0 of class B, with type 2,
  goes to state 3 (`319e20` → `31a0e0` → `28ef70`, **the catch**) if the result is 1, otherwise to 4 (end).
- Proof: with the flee swapped for victory in state 5 (what the mode does today when the player asks for the catch), the Rattata at full HP
  left the battle via RUN and the game opened the native catch ("Oh! You encountered a wild Rattata!" → caught).

**The wild Pokémon's HP and status.** The library object that goes to the catch (`[B+0xc0]`) does **not** receive the
battle's HP (21/21 after the wild Pokémon fainted). What holds the HP is the battle Pokémon (BTL_POKEPARAM): at the end of the
battle, victory **or flee**, `9e4280(bpp)` (ReflectToPP) writes back to the library, one per Pokémon (the player's and
the wild one last). Fields: `+0x60` the library object, `+0x78` species (u16), `+0x7c` max HP, **`+0x7e` current
HP**, conditions in the low 3 bits of `+0xa0` (paralysis), `+0xa8` (sleep), `+0xb0` (frozen), `+0xb8` (burn),
`+0xc0` (poison), in the library's order 1..5. Measured: flee with the Rattata at 10/21; victory with it at 0/20.
- Library (pml): `CoreParam` = object with the accessor at `+0x98`; accessor `[+0x10]` main block (u32 key at +0,
  0xE0 bytes encrypted starting at +8), `[+8]` battle block (0x1C bytes: u32 status, level +4, HP +8, max HP +0xA,
  like PB7 at +0xE8), encrypted with the same key while `[acc+0x18]` (LCG `0x41C64E6D`/`0x6073`, `739640`).
  Species `724950`, accessor GetHp `729ea0`, GetMaxHp `729fc0`, SetHp `730de0`, `CoreParam::SetHp` `7265d0`.

**The catch formula** (`7de080`, called every frame of the scene, also for the ring colour): reads species/form
(`+0`, `+4`), ball (`+0xc`) and berry (`+0x10`) by table, the throw multipliers (`+0x14..+0x20`), the level
(`+0x24`, up to 100) and **the species rate** via `71fc30(8)` → `71dfd0` (GetPersonalParam: `[this+0x10]` = 0x54-byte
record of `personal_total.bin`, **id 8 = catch rate**, byte +8). Pidgey/Rattata 255, Pikachu 190, Eevee 45,
Snorlax 25. Nobody else asks for the rate during the catch (logged with `LARECOMP_WILDBATTLE_CATCHLOG=1`). Note: the raw
copy of `personal_total.bin` in the heap (`0x729e0ea268`) is not read by anyone (read watch).

**The command menu** (frame "battle act select main frame", factory `aaa860` → `aaac10`, 0x190-byte object,
constructor `aab290`, vtable via GOT `0x7001608ed8`): `ab2620` is the loop; `ab1960` reads the event (1 = a `uikit` button
decided); `ab1af0` → `ab1e80` translates the **button id** (`[event+4]`, from `uikit_setting_battle_command_00.bin`)
into the action written at `[[...]+0x78]+0x10`: id 1 → 1 (Fight), 2 → 2, 3 → 3, 4 → 4 (Run), 5 → info, 6 → partner, 7 → 0xA.
A new id falls into the empty case. The layout (`bin/app/battle/bin/battle_command_00*.arc`, SARC with BFLYT and even the source XML)
has the panels `N_command_00..03`/`L_command_00..03` and the animations `command_patern_00/01`; `layout_setting` and
`uikit_setting` are FlatBuffers that link panels and ids by hash.

**The ball decision** (`7dcb00`, the throw's "judge"): `b = 65536 / (255/a)^(3/16)`, four checks with the scene's xorshift
(`[[+8]+0x720/0x728]`), the ones that passed in `[juiz+0x128]` (4 = caught; Master Ball and the case `[+0x748] == 3` force 4).
`7dd230` decides the next state (0 shakes → 6, otherwise 1). The scene plays the sequences by index (`80f510(cena+0x238, n)`;
13/14 = the Pokémon fleeing, normal or legendary).

**Leaving the scene without catching**: `919970` ("out of balls": the bag has none of items 4, 12, 5, 3, 2 or 1, via `1c9750`)
takes the scene to line 7 of `poke_get` ("You don't have a single Poké Ball left! You'll have to withdraw for now...") and to the native
end. The scene menu's "Run away" also ends it ("Do you want to give up catching this Pokémon?" → Yes).

**The lines** (`bin/message/<idioma>/common/*.dat`, encrypted GF text: key `0x7C89 + i*0x2983` per line, 3-bit rotation
per character): `btl_std` line 82 "You got away safely!", `poke_get` lines 1 ("Oh! You encountered a wild X!"),
2 ("Now's your chance! Catch the wild X!", the legendaries' one), 7 (no balls), 8–11 (broke free, depending on the shakes),
13 (fled), 14 ("You got away safely!"). Every line goes through `6786b0` (x0 = the file data, `[+0x90]`+2 = number
of lines, `[+0x98]` the line table, 8 bytes starting at +4; x1 = the encrypted line, w2 = length, w3 = index, x4 = the
buffer: u16 text at `+0x58`, capacity `+0x78`, length `+0x7a`). `btl_std` has 392 lines and `poke_get` 70 in all
languages. The object that arrives there is not the one from loading (`1afc00`).

**The 5th button (CATCH), native.** How the menu builds the buttons (all seen at runtime, Eevee and Pikachu):
- The language's `.arc` (`battle_command_00{,_eng,...}.arc`, SARC) has the menu layout (`blyt/battle_command_00.bflyt`) and the
  button's (`blyt/battle_command_button_00.bflyt`, a "part" used by `L_command_00..03`). Each button's appearance comes
  from the button's `patern_00` animation: frames 0 fight, 1 Pokémon, 2 bag, 3 run, 4 back (this one only in 2-player
  mode). It swaps the textures (circle `battle_command_*_off_00` BC3 100×100, icon `battle_button_command_icon_0N`
  BC4 200×200, labels `grpfont_battle_*Button_text_00` BC5 and `_text_01` BC4 160×128: the text is an image, one per language) and
  the material colours (glow and background layers: the material's white colour; label shadow: black colour; in `mat1`,
  `+0x24`/`+0x28` after the name).
- The code (`ab2730` → `ab2e60` for each of the 4 views at `menu+0x288..0x2e8`) finds the `uikit` button through a
  fixed table of 4 hashes (`0x7000f84c50`), applies the pattern animation (hashes fixed in the code) and writes into the button the
  decision callback (`[botão+0x150..0x168]` = `ab49f0`/`ab4a20`/`ab4a30`/`ab4a50`, `+0x170` the view, `+0x178` the event):
  on decision, `{1, tabela 0x7000f98258[quadro da vista]}` → ids 1..4 and 7; `ab1e80` translates the id into an action.
- The 4 buttons are positioned by the code from the right edge of the `B_bounding_00` area (RUN = edge − 66, the others
  every 136); the `uikit` navigation grid has the number of columns in field `f1` of the navigation table (4).
- In trainer battles the code hides RUN (`34e8b0` by hash) and keeps 3 buttons.

What the runtime does (`runtime/hle/catch_button.c`, from the player's own files; `tools/catch_button.py` is the
same in Python and `tools/catch_button_art.py` draws the art, embedded in `runtime/hle/catch_button_art.h`):
- textures of the BACK button redrawn as CATCH (same size and format; `stb_dxt` compression, Tegra swizzle);
  labels CATCH / CAPTURE / Fangen / CATTURA / CAPTURAR / つかまえる / 잡는다 / 捕捉 (with a small "CATCH" in the Asian ones);
- `blyt/battle_command_button_catch.bflyt`: a copy of the button layout pointing to those textures, in red, without the
  pattern animation; in the menu layout, `N_command_04`/`L_command_04` in place of RUN, and the 4 buttons and the
  `B_bounding_00` area one step (136) to the left;
- `layout_setting`: the 20 new panels and the 6 animations of `L_command_04`; `uikit_setting`: the 5th button (RUN's with the
  derived hashes: FNV-1a 64 of `"%016x:catch"`) and the grid's 5th column (5 columns). FlatBuffers only get additions at the
  end (offsets point forward: lists that grow are copied whole);
- `runtime/services/fs.c` registers the 11 files with two contents: the assembled one, delivered while a battle of the mode
  is running (`wildbattle_menu_active`), and the original padded with zeros to the same size in other battles;
- `views_hook` (`ab2730`) writes into CATCH the same callback with its own view of frame 5 (id 0, which the game ignores);
  `invoke_hook` (`ab49f0`) notes that it was CATCH and `decide_hook` (`ab1e80`) turns it into a catch request.

**Implemented** (`runtime/hle/wildbattle.c`, "Catching during the battle"; tested on Eevee and Pikachu, save
`build/save-edit`):
- CATCH → the battle ends as a flee, which state 5 of the event swaps for victory → the native catch;
- the wild Pokémon's HP, max HP and status saved in `9e4280` (id `+0x87` ≥ 6); the species rate the formula asks for
  (`71dfd0`, id 8) becomes `taxa × (3·máx − 2·vida)/máx × (2,5 sono/congelado; 1,5 paralisia/veneno/queimadura)`, up to 255.
  The scene's ring changes colour along with it (seen: red with rate 1);
- fainted (victory with HP 0) → the result becomes a flee → no catch;
- one ball at a time: after a ball that did not catch (`7dcb00`), `919970` answers "no balls" → the scene closes
  by itself → **the battle again** (from the world loop, `2f78d0`, with the screen still dark), with the HP and the status blocks
  put back into the battle Pokémon when it is set up (`9e2490`, BTL_POKEPARAM::Setup). Seen: Pidgey 3/17 →
  failed catch → rematch with the bar at 3/17 → fainted → no catch;
- lines: "Time to catch it!" in place of the battle's "You got away safely!"; "Now's your chance! Catch the wild X!" (the
  native line 2) in place of "Oh! You encountered..."; "It's still raring to go! Back to the battle!" in place of the
  "no balls" line. Our own texts in the game's 9 languages (`k_catch_text`).

**New tools** (docs/DEBUGGING.md): entries `b<N>:<botão>:<len>` and `c<N>:...` in `LARECOMP_INPUT` (from the start
of the battle / of the catch) and `LARECOMP_FNPROF=b<de>:<até>`; `LARECOMP_WATCH_READ=1` watches reads; `LARECOMP_WILDBATTLE_*`:
`STATE_LOG`, `CATCHLOG`, `HPLOG`, `HPSCAN`, `MENULOG`, `MSGLOG`, `AUTOCATCH`, `FORCE_RATE`, `CATCH_MENU`, `CATCH_DUMP`.

**Adjustments after the user's first test** (23/10/2026; details in HISTORY):
- position: the `B_bounding_00` area stays where it is (with 5 buttons the game already starts one step earlier): CATCH ends where
  RUN used to end;
- no clock: `28c770` (static battle setup) writes 300 s to the event's `+0x562`; the hook zeroes it only in ours.
  Bit `0x80` of `+0x574` stays: without it the wild Pokémon faints the way it does in trainer battles, whose sequence looks for
  a nonexistent model (`a9c700`);
- XP: `9d8ae0` (the "XP enabled" check: competitor ≤ 1, setup `+0x4a4` zero, no `0x80`/`0x800` in `+0x494`) answers yes
  in our battles: the knockout gives the native XP (line 44/46 of `btl_std`, bar, level up) and the battle ends;
- the catch's "Run away": lines 24/4/14 of `poke_get` swapped in our catch ("Back to battle", "Do you want to go back
  to the battle?", "Back to the battle!");
- going back to the battle: `ee330.bseq` (the wild Pokémon's entry camera) of 1 frame, delivered by `fs.c` only on the return
  (`wildbattle_quick_intro`), and the "A wild X appeared!" window does not open (`aaf230`); the send-out (`ee420`, or the shoulder
  partner) stays whole, but hidden: fade-in held, no frame pacing and no sound until the menu (`ab2730`);
- "Back to battle" without confirmation: the catch exit dialogue (`7d2380`) goes from state 0 (`7de440`, the question)
  straight to 1 (`7de7d0`, the exit); the runtime presses A on the next line (`input_press_a_at`);
- textures with mipmaps (the PT-BR mod): all levels rewritten;
- after a ball that bursts, the wild Pokémon acts first: the return menu is answered in hiding with a flee that fails
  (`9f7300` answers no; line 83 does not open), so the turn is only its own;
- catch chance: the HP/status factor goes into the formula's result (`7de080`: `taxa^0,54 × ...`, up to 255), not into the
  species rate;
- CATCH with the Pokémon trapped (Wrap etc.): the client's flee check (`a5c350`) answers yes;
- evolution: after one of our knockouts, the post-battle (`319bc0`) goes from state 0 to 1 (the trainer battles' evolution loop:
  `728300` checks, `83cef0` opens the evolution event) instead of 4 (end).
New addresses on Pikachu: `28c770` → `28c740`, `9d8ae0` → `9d8af0`, `7de080` → `7de0b0`, `a5c350` → `a5c360`,
`aaf230` → `aaf240`, `319bc0` → `319b90`, `7de440` → `7de470`, `9f7300` → `9f7310`. Tested by script in both games.

**The starter walking alone** (24/10/2026, issues #6/#22; HISTORY): it also battles. In our battles, with it alone,
`a7e090` ("is it the starter?") answers no, so it comes out of a Poké Ball, and `a99b40` (the battle model's Pokémon)
receives form 0: the species' model instead of the field model of the one walking along, which the static battle does not have.
Pikachu: `a7e090` → `a7e0a0`, `a99b40` → `a99b50`.

**Fainting and music** (24/10/2026; HISTORY): bit `0x80` of `+0x574` is cleared when the menu appears, and the defeated wild Pokémon
faints as in trainer battles (`ee620`; `a9c700` answers "no joint" to a model with key 0). The music:
`16d060` (Wwise events by name) does not send the `Pop_BGM_battle_to_field` of a catch that returns to the battle nor the
`Push_BGM_field_to_battle` of the return. Pikachu: `a9c700` → `a9c710`, `16d060` → `16d030`.

**Missing**: with the PT-BR translation mod CATCH shows up as a black square (HISTORY); the ~12 s wait between choosing (CATCH or the game's RUN, the same) and the battle ending; nature/shiny/ability
of the touched Pokémon; seeing a real trainer battle with the original menu (tested only with `CATCH_MENU=0`); the
in-game labels in other languages (checked by decoding the assembled files, `CATCH_DUMP`).

## Next steps
_Updated on 17/10/2026: items 4 and "turn on/off in the launcher" from 5 are done (Step 4 above, option
"Wild battles (experimental)" since 0.2.0, in both games since 0.2.1). The rest of 5 is still missing (split XP, fleeing,
the Pokémon disappearing from the map) and 3._

3. Confirm the alternative path (`289c4c` → `28bda0`, mode 3) and the post-battle catch (`319e20` → `31a0e0`) at
   runtime: ideally a save near a native wild battle (legendary or Snorlax); without that, force the
   method and observe.
4. **Prototype**: on the wild encounter (`305ad0`), instead of `28ef70`, build the wild battle request for the
   touched Pokémon and start the battle; at the end, the catch.
5. Details: fleeing, experience, the Pokémon disappearing from the map, turning the mode on/off in the launcher.
