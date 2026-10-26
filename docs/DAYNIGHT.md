# Day / night

Status (26 October 2026): scene tint by the clock (overworld only, UI untouched) and a first night-lights demo (glass, signs, lantern) work; see below.

## What exists
- `runtime/daynight.c`: hour -> tint, brightness, `daynight_night()`, and the glow table. `larecomp.ini`: `daynight = 1`,
  `daynight_speed = N` (1 = real time). `LARECOMP_HOUR=<0..24>` forces an hour (also with the setting off).
- Scene tint (`runtime/nvn/vulkan/exec.c` `ssao_run`, end): the scene is multiplied by the hour's tint after the scene pass, before the
  interface is drawn; only while `fps60_world_active()` (no title, battle or capture). Hues / timeline follow `ssrecomp`
  (night 0.40 / 0.45 / 0.70; full 20:00-4:00; dusk from 17:00).
- **Night lights** (`tests/runs/daynight/texkeys_demo.png`): a draw that samples a glowing texture is multiplied by M / T through the blend
  constant (`glow_draw` in exec.c), so after the scene tint T it ends at M x its daylight colour - brighter than the dark town, and over
  the game's bloom threshold. The glowing textures are identified by content key (`tex_content_key`: FNV-1a over the first
  min(16 KB, level-0 bytes) of texel data, as the game laid it out - identical in every run and area) and named from the game files:
  lantern glass `88de1b27` (`lamp_glass01_area02`), Center windows `74e4fa01`, Mart windows `31d4c774`, Gym windows `00f5e95f`, house
  windows `7d5117e6` (`window01_com`), Center roof sign `abc4c88f`. Table in `runtime/daynight.c`. Because each glass material is its own
  texture and its own draw, nothing spills onto walls, frames or other objects (compare `LARECOMP_GLOW_MODE=color`, the older colour key
  on the finished picture plus a light spill, which lit whole facades, foam and clothes).
  `LARECOMP_GLOW_TEX=<hex key>,...` adds keys (warm) for testing; `LARECOMP_GLOW_DEBUG=1` (colour mode) paints matches magenta.
- Known limits: Pallet's house windows are not lit yet (their texture is not named yet); the lantern has no light on the ground; windows
  of other towns need their keys; blended draws are left alone; interiors and cutscenes are still tinted (the game reports them as
  overworld); no in-game menu entry; GPU cost not measured.

### All the towns, doors, interiors and dynamic lights
- **Table from the game files.** `bntx_scan.py` reads every texture of every field pack (9,853: name, size, format, runtime content key; only
  BC1 sRGB 0x1a06, BC3 sRGB 0x1c06 and RGBA8 0x0b01 have a known runtime format, which is all the glass uses), `gen_glow_table.py` picks the
  outdoor windows (all towns, the Center / Mart / Gym, GOGO park), glass doors (the Center's Pokeball door `bldg_pc_door01_com` and the
  blue sliding door `bldg_gym_door01_com`), lamp glass, street lamps and the Center / Mart roof signs -> `runtime/daynight_glow.inc`
  (28 textures), classes and colours in `runtime/daynight.c`. The Mart's door texture (`bldg_shop_door01_com`) looks like dark metal in the file, but
  the game's door shader renders it as pale glass: it is its own draw (found by skipping draws, `bisect_draw.py`) and is in the table now.
  Judge a texture by how it renders, not by the raw file. House windows use
  `window01_com` and light up everywhere it is used. Not covered: windows painted into other textures, textures in other formats.
- **Interiors.** `runtime/services/fs.c` notes which light set the game last read (`field/draw_env/default|room|dungeon`, logged as
  `[fs] light set: ...`); `daynight_tint` returns false for `room` and `dungeon`. `default` is confirmed in the overworld (Pallet, Viridian);
  that an interior really loads `room` is NOT verified yet (no walk into a building was run).
- **Dynamic lights** (off by default since 2026-10-26, pending retest; `LARECOMP_LIGHTS=1` turns them on; `LARECOMP_DL_GAIN`, default 2.5; `LARECOMP_DL_ZW` depth window; `LARECOMP_DL_FLIP`).
  In the night pass (`ssao_run`): the lit pixels (pre-tint brightness above a threshold: glowing draws are boosted by M / T) are reduced to a
  1/4-size light map with each source's colour (its own hue, half toward warm), intensity and view depth (`dl_extract.frag`), a 4-level
  blur pyramid is built for the halo (`dl_down.frag`), and `dl_apply.frag` multiplies the scene by the tint x (1 + the light reaching the
  pixel): a pool = 32 taps of the map over a disc of ~380 cm, looked up where the source is above the pixel (a lamp 230 cm up), each tap kept
  only if its source depth is within ~380 cm of the pixel's, plus a halo around sources. The ground keeps its own colours (a multiplier).
  Limits: subtle at the default gain (stronger with `LARECOMP_DL_GAIN=5`), a little grainy up close, the pool is an approximation (no
  normals, walls and ground are lit alike), the in-between images of frame generation get the plain tint, extra cost (~6 passes at
  scene size and below) not measured. `tests/runs/daynight/demo_dynamic_lights.png`.

### Colour key inside a draw (unused so far)
`ShaderKey.glowck` + a uniform at binding 21 (translate.c, `glow_ck_draw` in exec.c, `daynight_glow_colorkey`): the fragment shader multiplies
pale-cyan pixels of a listed texture's draw by M / T - for glass that is only a region of a bigger texture. The list is empty: every glass
found so far is its own texture and draw.

### Strength, hues and ray-traced shadows (reference: Sword / Shield night shots)
- Night is bluer (tint 0.50 / 0.60 / 1.0, brightness 0.70, saturation 0.85) and the glows are lower so panes, frames and bulbs keep their detail:
  windows x(1.7, 1.05, 0.4), glass doors (0.95, 1.25, 1.55), lantern bulbs (2.2, 1.45, 0.5), signs (1.7, 1.15, 0.95). `LARECOMP_GLOW_STRENGTH`
  scales how far each goes past daylight (0.5 = half). Pools: `LARECOMP_DL_GAIN` default 1.8 (was 2.5).
- Dynamic lights now use one 1/4-size light map (colour, intensity, source depth) read by nearest texel; the blur pyramid was dropped.
- Ray-traced shadows: `LARECOMP_LIGHTS_RT=0` turns them off. This is a screen-space ray march (7 steps from the pixel to its light through the depth
  buffer; a point 20-450 cm behind the surface blocks the light, factor 0.15) - not hardware ray tracing. In the test scenes its effect is
  small (mean pixel change ~1 / 255 near a lamp), because the pools are faint and few objects stand between lamps and ground.
- Real ray tracing (Vulkan ray query on the RTX card) would need the town geometry in an acceleration structure: the meshes are drawn from
  guest memory each frame, so it means capturing every opaque draw's vertex / index data into BLASes each frame (or caching static blocks)
  and a TLAS from the draws' matrices - days of work and a GPU-cost risk; not started. The screen-space march is the cheap stand-in.
- `LARECOMP_DL_DEBUG=1` paints the light-map lookup (research).

### Blocky pale halo around windows at night (open)
Seen in Pewter (`tests/runs/daynight/results/halo_*.png`, `artifacts_check.png`): stair-stepped pale-blue steps (~6 px at 720p) around lit windows.
Not caused by the ray-traced shadows or the dynamic lights (identical with `LARECOMP_LIGHTS=0`), not by my glow (identical with
`LARECOMP_GLOW_STRENGTH=0`), not by the game's bloom (its levels are smooth; adding the composite shader `391277e86e32a724` to the FX_UP table
changed nothing and was reverted), and absent by day. Skipping draws in that region (`bisect_draw.py`) points to a depth-only draw
(fragment hash 0), i.e. the shadow map: most likely the shadow edge of the window recess, blocky at the shadow map's texel size and only visible
once the tint darkens the rest. Not confirmed. Candidate fixes: fade the directional shadow's strength toward night, or blur it.

### Wide view for checking lights (`LARECOMP_ZOOM`)
`LARECOMP_ZOOM=<0.2..4>` scales the field of view of the main scene (below 1 = zoomed out; 0.45-0.5 shows a good part of a town). It is a
`gl_Position.xy *= zoom` in the vertex shaders of the draws into the main scene target (`ShaderKey.zoom`, translate.c, set in `draw_`
like the TAA jitter); the scene block's matrices are not what static meshes use, so patching them did nothing. Limits: the game still
culls with its own camera (holes near the edges, black strips), very low values show the sky dome, and shadows / SSAO are not adjusted.
`tests/saves/viridian-tour/c1..c3` are positions around Viridian (Mart, Gym, houses); `tests/runs/daynight/viridian_tour_night.png`.

### How the keys are found (reusable for every town)
1. `romfs_ls.py` lists the game's files; `gfpak_extract.py` unpacks `/bin/archive/field/area02.gfpak` (328 compiled shaders + 212 BNTX
   textures, names inside) and a town pack (`town002_00.gfpak`: file 0 is the model, `gfbmdl_materials.py` lists its materials,
   shaders and texture names).
2. Run the game with `LARECOMP_DRAWLOG=<present>`: every scene draw is logged with its textures (`kXXXXXXXX nBYTES` = the content key).
3. `bntx_key_match.py <log> <unpacked pack> 328` recomputes the key from each BNTX file and names the runtime textures
   (`tests/logs/vir-texture-names.txt`); for a texture it misses, find its BNTX by name and try the dimensions / bytes of the log.
4. Add the key to the table. To see what a draw is: `LARECOMP_ONLY_DRAW=a-b` (only those logged draws) or `LARECOMP_SKIP_DRAW=a-b`,
   with the shot taken at the next present (`SHOTS=1601`); `bisect_draw.py` automates the search.

### The lamp, as the model and the runtime see it
- Model: materials `lamp01_town02_area02_2` (`FieldObjectShader_Emission`: body / light fitting; textures `lamp01_town02_area02`,
  `light01_com` = `e6ce08a6`) and `lamp01_02_area02_6` / `lamp_glass01_area02_2` (`FieldWindowShader`: the glass; texture
  `lamp_glass01_area02`, 64x64).
- Runtime (Viridian frame): the pole and fitting are one draw (fs `3b6ffffc84936523`, shared with a few other props); the glass panes are
  another draw of 168 vertices with the 64x64 glass texture only (`88de1b27`). Boosting the glass texture lights exactly the panes.

## Research tools (all ignored by git under `tests/`)
- `LARECOMP_ONLY_DRAW=<a>-<b>`: with DRAWLOG, only those logged draws are drawn (`bisect_draw.py`). Textures are saved as
  `<addr>_<w>x<h>_k<content key>.png`.
- `LARECOMP_DRAWLOG=<present>`: logs that present's scene draws (fragment shader hash, blend, depth write, args, textures) and
  saves each texture once as `build/tex/<addr>_<w>x<h>.png`. `LARECOMP_SKIP_TEX=<addr>,...` leaves out the draws that sample them.
- `tests/scripts/run-daynight.sh <hour>` (env `W`, `EXTRA`, `TAG`), `contact_sheet.py` (thumbnails of a texture folder),
  `bisect_tex.py <x0,y0,x1,y1>` (which texture draws a screen region).

## Findings
### From the town models (static; `tests/scripts/romfs_ls.py`, `fbdump.py`, `gfbmdl_materials.py`)
- Each town is a pack `/bin/archive/field/area0N/townNNN_00.gfpak` (`GFLXPACK`, entries LZ4, type 2). Viridian is `town002_00`
  (gym, Pokémon Center, Mart, houses, lamps); Pallet is `town001_00`. File 0 of a pack is the `.gfbmdl` (FlatBuffers);
  extracted to `tests/runs/models/town002/` (`file0.bin`, `file0_tree.txt`, `file0_strings.txt`).
- The model names its materials and each material's shader. Viridian: `FieldWindowShader` is shared by every window
  (`bldg_gym_window01_com`, `bldg_pc_window01_com`, `bldg_shop_window01_com`, `window01_com`, `lamp01_02_area02_6`, ...);
  `FieldObjectShader_Emission` is the lamp head (`lamp01_town02_area02_2`; slots Texture01 + Reflection_tex; float params
  Hilight_Strength, RimLight_*, Reflection_Level; colour `lightColor`); signs are `bldg_pc_sign01_com`, `bm_signboard*`;
  doors `doorway01_com` (slot `LightMask_uv1`); roofs use `FieldDefaultShader`, walls `FieldObjectShader`.
- The game has no time-of-day lighting: `/bin/field/draw_env/{default,dungeon,room,test}/Env{Amb,Dir}Light0-3.gfblt` (88-96
  byte light tables) and `EnvShadowLight.gfblt` are fixed sets. `room` vs `default` is a candidate indoor / outdoor flag.
- So window glass IS a separate material with its own shader (my earlier "painted into the atlas" conclusion was wrong: the
  texture-skip test was dominated by the shared 1024x1024 world texture `72dc33d000`, which 94 draws sample).

### From runtime tests (Pallet Town, `tests/runs/daynight/`)
- (Superseded by the texture-key method above.) Skipping fragment-shader hash `dfe6f535cb747369` (44-ish draws of the FieldObjectShader family) removes wood trim, balconies
  and window frames but leaves the glass pane, so the glass is another shader. Its hash is not found yet: the bisection was
  confounded by draws that overlap in the region. All blended hashes were ruled out (the glass is an opaque draw).

### Moving the player (save tool, `tests/scripts/save_pos.py`)
- Save layout = PKHeX SAV7b: "Coordinates" block at 0x47A00 (0x6010 long): X +0x10, Z (height) +0x14, Y +0x18 floats, rotation
  quaternion +0x30, map id u64 +0x6000. Block checksum = CRC16 (poly 0xA001 reflected, init 0) in the footer at
  0xB8618 + 8 * block + 2. The tool prints / edits these and fixes the CRC (checked against the real save).
- World layout: `tests/scripts/area_positions.py` -> `tests/logs/area-positions.txt`: min/max box of every town and road
  (x, height, y; same units as the save). Pallet x -2200..5801, y -418..3713; Route 1 y -4038..66; Viridian (`town002_00`)
  x -310..5700, y -8767..-3605 (north = negative y). Changing only X/Y/Z moves the player; the map id may stay.
- `tests/saves/viridian-lamps/` = Viridian with a lamp post, pond, benches and the Mart in view (x 1500, y -5000, z 0).
  `SAVE_SRC=tests/saves/viridian-lamps TAG=vir bash tests/scripts/run-daynight.sh <hour>`.

## Next
1. Tie shader names to runtime fragment hashes: per material, the model gives the shader and texture names; match the
   textures (bntx in the area packs) to the runtime ones by content, or find the hash by skipping candidates one at a time
   in a scene where only the glass is on screen.
2. Lamps: `FieldObjectShader_Emission` draws are the lamp heads; the draw's model constants give the world position.
3. Windows / signs: `FieldWindowShader` draws -> brighten and glow at night (a per-draw multiply that the scene tint skips).
4. Pixel-diff bisection (`bisect_tex.py`) is unreliable in towns: the camera and NPCs drift between runs. Identify draws by
   rendering only chosen draws (a draw-index filter, not yet written) or by vertex-buffer content hash.
5. Light map pass and interiors (`room` light set) as before.

## Halo fixed (dynamic lights)
The blocky halo came from `dl_apply.frag`'s halo: 9 fixed taps on the nearest-texel 1/4-size light map (4 px steps, no noise). Now 12 noise-turned taps, bilinear between 4 texels (`lmb`). Proof: `LARECOMP_LIGHTS=0` removed it (`tests/runs/daynight/results/halo_nolights.png`), fix in `halo_fix1.png`.
Earlier "ruled out" tests were invalid: `SAVE_SRC` must be the folder CONTAINING `save_0` (`tests/saves/play-pewter`), not `save_0`, else the run is a new game. Also bash drops `VAR=x cmd` prefixes here: use `export`.

## Grain fixed (dynamic lights, pool)
The "weird artifacts" on walls/ground were the pool's 32 noise-rotated taps (salt-and-pepper grain, `results/grain_lights_on_off.png`). The pool now reads a depth-aware 4-level blur pyramid of the light map (`dl_down.frag`, `lvb()` in `dl_apply.frag`): `results/grain_before_after.png`. Screen-space shadow march (`vis`) is no longer used in the pool.
Blur widened: pool uses pyramid level from radius/2; halo = levels 2+3 (results/blur_more.png). Random-tap halo removed (it grained the wall).

## ssrecomp-style extras (in `dl_apply.frag`)
- Vignette: edges up to 20% darker at full night (ssrecomp's numbers).
- Fireflies: 3 layers of screen cells, ~45% hold a yellow-green dot drifting and blinking (time = SDL ticks, push d.x), only where the scene under it is green (grass, leaves). `LARECOMP_FIREFLIES=0` turns them off. Shot: `tests/runs/daynight/h22_1700_ffx_window.png`.

## Door, pool spill, moon on water
- Glass doors: warm-white {1.25, 1.12, 0.85} (cool x blue texture read as a cyan screen). Light-map intensity capped at 1.2.
- Pool: 4 lookups 40..280 cm above the pixel, nearer weighing more (was one 230 cm shift: a lit facade became a flat rectangle on the ground); max pyramid level 2.
- Water (vivid blue, unlit): silver sheen + smooth ripple streaks along a moon column (screen x 0.64), broken by value noise. Random per-pixel glints removed (grain). Shots: `tests/runs/water/`, `tests/runs/waterdoor/_sheet.png`.
- Light sources must be warm (r >= 1.15 b): white caps, water foam and glints no longer make pools (pink patch on water). Pool looks 0..150 cm up (mostly radial: the 40..280 cm version drew beams down the screen). Moon ripples lower frequency (no aliasing dots).
