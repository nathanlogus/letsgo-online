- Cloud layers, after EntroPi's Unity Cloud Shadows (Apache-2.0; the idea, not its code or textures): two layers - big slow clouds
  (16 m, their outline warped) and smaller wisps (6.5 m) drifting faster another way at 45 % - each set by coverage and softness
  with its formula (`step = -soft + (1 - cover)(1 + 2 soft)`, a smoothstep around it), composited as "multiply inverse". Projected
  along the real sun: the shadow map's light (`sun_dir`, now probed whenever day / night is on), a fixed slant without it.
  `LARECOMP_CLOUDS` strength (default 0.4), `LARECOMP_CLOUD_COVER` 0..1 (default 0.5). `results/cloud_layers_strong.png` (at 0.9).
meline follow `ssrecomp`
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
- **Dynamic lights** (on by default; `LARECOMP_LIGHTS=0` turns them off; `LARECOMP_DL_GAIN`, default 2.5; `LARECOMP_DL_ZW` depth window; `LARECOMP_DL_FLIP`).
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

## Settings from DQ HD-2D's "Dynamic Lighting Improvements" (2026-10-26)
That mod (gamebanana 637365, Dragon Quest I & II HD-2D, Unreal) is only engine settings (`[ConsoleVariables]` in DefaultEngine.ini,
copy in `tests/reference/dq-dli-mod/`). Three ideas ported to `dl_apply.frag`, each with a switch:
- `LARECOMP_DL_CONTRAST` (0.7; 1 = off): its `r.Color.Mid=0.7` - at night the dark and middle tones go down to 0.7, the bright ones
  (pools, windows, lamps) stay: more contrast between lit and unlit.
- `LARECOMP_DL_STREAK` (1; 0 = off): its `r.Bloom.Cross=4.5` - 4 soft rays from each light (light map level 1 read along the row and
  column, 8 steps of 10 px at 720p, no depth test).
- `LARECOMP_DL_FAR` (2.5; 1 = off): its `r.LightMaxDrawDistanceScale=2.5` - the pools' depth window widens up to 2.5x from 15 m to
  60 m, so far lamps keep their pools.
Not ported: its shadow, AO, fog, DOF and resolution settings (the game's own renderer, or already ours).

## Fireflies in the world, moon off, no black frames (2026-10-26)
- Fireflies: own pass `dl_ff.frag`, placed in 1.5 m world cells around the visible surface (depth -> inverse of this frame's
  view-projection `ao.vp`) and projected back: they stay put as the camera moves (they slid with the screen before) and hide
  behind walls. Shots: `tests/runs/daynight/results/walk_fireflies*.png`.
- Moonlight on water: off by default (`LARECOMP_DL_MOON=1`). Its colour test also took blue roofs for water; a real fix would tag
  the game's water draws like the glow textures.
- Black frames while walking (reported, not reproduced): a NaN / inf from the light pass would be spread over the whole frame by
  the game's bloom. The light map drops non-finite depths and both passes replace a non-finite output.

## Moon on the water only (2026-10-26)
- The water's draw is found by its fragment shader (`0x35899cd94b78efd3`: Viridian's pond and route 21's sea; bisected with
  `THRESH=30 tests/scripts/bisect_tex.py ... fs` - the water animates, so the default threshold 6 always reads "changed").
  At night it gets a shader variant (`ShaderKey.glowck = 2`) that adds the sheen and ripple streaks inside the water itself:
  no colour guessing, roofs stay dry. `LARECOMP_DL_MOON=0` turns it off; `LARECOMP_WATER_FSHASH=<hex>,...` adds other water shaders.
- Bug found on the way: `get_program`'s cache key left out `glowck`, so its variants got the plain program - the glass colour key
  (`glowck = 1`, Mart door glass inside its wall texture) never applied before either. Now it is part of the key.
- `LARECOMP_SAVE_MSL=1` now saves variants as `<addr>_fs_water.glsl` / `_fs_glass.glsl` instead of overwriting the plain one.
- Shots: `tests/runs/daynight/results/moon_water_only.png` (pond, Pallet, route 21).

## Celadon and Saffron windows (2026-10-26)
Their window glass textures are nearly black (the blue is a reflection map), so the glow multiplier kept them dark. They now get a
warm light added in their own shader (`ShaderKey.glowck = 3`, `daynight_glow_emissive`, linear {0.6, 0.3, 0.08} x night before the
tint): `window02_01_area07`, `window03_01_area07`, `window01_com` and `a45b4798` (Celadon's blue windows: not in the name index,
found with `bisect_tex.py` after removing the textures every building draw shares - the 256x8 ramp, the 2048 / 1024 maps - from the
candidate list). Shots: `tests/runs/daynight/results/celadon_saffron_glass.png`, `celadon_glass2.png`.

## Route lamps (2026-10-26)
The curved route lamps (routes 5, 6, 7, 8, 24, 25: `buildmodel` `streetlight01_0x`) were all `GL_LAMP_POST`, a faint warm lift.
Their light panel is its own draw (fs `dfe6f535cb747369`, texture `streetlight01_02` `4d78e548`), so it is now `GL_LAMP_GLASS`
(`gen_glow_table.py`): the panel glows and the night lights give it a halo and streaks; the pole keeps the faint lift.
Shots: `tests/runs/daynight/results/route_lamp_head.png` (route 7: fix | panel skipped), `route5_lamps.png`.
The lamps sit outside the default camera view on most routes; the shots use `LARECOMP_ZOOM=0.5`.

## World tour: town lamps, Cycling Road, battle intros (2026-10-26)
`tests/scripts/world_tour.sh 22` (every town and route box, wide view) -> `tests/runs/towns/`, `tests/runs/routes/` (sheets `_sheet_*.png`,
made by `sheet.py <out.png> <img>...`). What it found and what changed:
- Town lamps: their glass is the lamp's second texture, a gray glow mask drawn on its own: `streetlamp01_01_area04` (Cerulean's
  globes), `lamp01_02_town005_area08` (Vermilion), `lamp01_02_town006_area09` (Fuchsia), `lamp01_02_town007_area06` (Lavender's
  bells) are now `GL_LAMP_GLASS`. Pewter and Viridian already were (`lamp_glass01_*`). Shot: `results/town_lamps_before_after.png`.
- Cycling Road's lamps (`roadlight01_area05`) are one texture, pole and head: the colour key has a second mode (`gck.w` 2, in
  `daynight.c`'s `colorkey_tex`) that lights only the white half of the head; the pole no longer gets the lift (it read neon orange).
  Shot: `results/cycling_road_lamp.png` (before | after).
- Battle intros were in daylight: the scene hook (tint, night lights) waited for 16 opaque draws and the intro draws 6 (the
  Pokémon, the water, the backdrop). With day/night on it now starts after 4 (`ao.min_opaque`; SSAO / TAA keep 16 otherwise).
  Shots: `results/battle_intro_before.png`, `battle_intro_night.png` (route 19, frames 1500-1880; the white flash is the game's).
- Dark glass (`glowck` 3) now also drops most of the pane's colour before adding the warm light (the reflection read pink in Celadon).
- Still open: the green panes of the house south-west of Pewter's museum (`tests/saves/town-tour/pewter`, wide view, bottom left).
  By day they are cyan glass; no glow-table texture draws them (removing the gym / shop / Center window textures' draws leaves them,
  and the removal bisect found nothing), so their colour comes from the game's own shading of that glass at night.
  Also ruled out (each tried as dark glass, no change): `bldg_gym_door01_com`, `bldg_pc_window01_com`, the mint pane texture
  `a69fbb4d` of the gym's draws. Since removing any logged texture leaves the panes, they are likely drawn without the depth
  test: `LARECOMP_DRAWLOG` does not log those and the glow paths skip them. Next: log the non-depth draws of that frame.
- Menus checked at night: the party screen keeps the tinted world behind it and a clean interface; the summary's 3D Pokémon is
  untinted (`tests/runs/daynight/menus/`). The lower scene threshold (4) did not reach them.
- Many route boxes put the camera in tree tops or out of bounds (`area02_road015_2`, `018_2`, `area03_road002_2`, `area05_road016_1`,
  `area06_road016_2`, `area08_road012`, `013`, `015_1`): the tour's drop point, not the lighting.

## Cloud shadows (2026-10-27)

- Soft cloud shadows drift over the outdoor ground, after the tint in `ssao_run` (`clouds.frag`, a multiply pass). The pixel's
  surface point (depth -> inverse of this frame's view-projection, as the fireflies) is moved along the sun by its height, then
  two octaves of value noise (clouds ~12 m across) scroll with the wind at ~40 cm/s.
- Strength: `LARECOMP_CLOUDS` (default 0.3 = 30% darker under a cloud; 0 = off), a third of it at night. Fades out from 30 to
  50 m of distance, so the sky dome and the battle backdrops' far mountains stay clean (at 8000 their polygons showed).
- Same gate as the tint: day/night on, outdoors only (no interiors or caves), the battles included.
- `LARECOMP_CLOUDS_DEBUG=1`: a 1 m world grid; `=2`: 2 m world cells, each its own tint (both should stay on the ground as the
  camera moves).

## Fireflies and clouds in the world, Pokémon Center roofs (2026-10-27)

- The fireflies and the clouds rebuilt the world position with the image's rows flipped (`nvn_window_origin() == 0`). Row 0 is
  NDC y = -1, as the motion vectors (`mv.frag`, validated) have it. Flipped, the positions followed the camera: the flies slid
  against the world while walking, and every size measured from them came out ~3x too small (the cloud scale was tuned on that,
  now redone). With the fix a cell grid stays on the path and cliff edges as the camera moves (`LARECOMP_CLOUDS_DEBUG=2`), and
  the flies move with the ground (`LARECOMP_FF_DEBUG=1`: every fly lit, 3x size).
- Still open: a pixel looks for flies only around its own surface point, so a fly in front of a roof edge can be cut.
- The Pokémon Center / Mart roof textures (`bldg_pc_roof01_02_com`, `bldg_shop_roof02_01_com`) are whole roofs, not lit strips:
  in the SIGN class they turned the roof bright red at night and the dynamic lights took it as a source (a halo over the
  building). Out of the table now; the Center's sign keeps its glow. Image: `results/pokecenter_night_fixed.png`.
- Center / Mart doors: the Center's blue glass door showed a ragged bright patch at night, and the Mart's white one blew out.
  The patch came from the dynamic lights (gone with `LARECOMP_LIGHTS=0` only, not with the moon or the glow off): the bright
  door was taken as a source and its quarter-size light fell back on it. `dl_extract` now skips cool pixels (`b > 1.25 r`;
  lamps and windows are warm), and the glass-door glow is lower (1.12 / 1.0 / 0.72 from 1.25 / 1.12 / 0.85).
  Image: `results/doors_night_fixed.png`.
- The doors still broke up while walking (60 fps, TAA): a stepped dark / bright edge over the Center's glass that crawled. Not
  the lights' screen-space shadows (`LARECOMP_LIGHTS_RT=0`: the same). `dl_apply` gave the pools and halos only to pixels below
  `lit 0.02` (luminance ~1): the lit glass sits at that brightness, so its texels were split into lit and unlit. Now a pixel
  takes `1 - smoothstep(0, 0.4, lit)` of them. Four frames walking past: one smooth door (`tests/runs/daynight/doorwalk/`).

## Real ray-traced light shadows: assessment (2026-10-27)

- Hardware: the RTX 3060's driver (621.1, Vulkan 1.4) has `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
  `VK_KHR_ray_tracing_pipeline`, `VK_KHR_buffer_device_address` and `VK_KHR_deferred_host_operations`. A ray query from
  `dl_apply.frag` (one shadow ray per pixel toward its light) would be the simplest use.
- Missing: the scene's triangles in an acceleration structure. Nothing captures geometry today (no transform feedback,
  no device address on the vertex buffers, the device is created with the swapchain extension only). The game's draws come
  from guest memory with their own vertex formats; static meshes, skinned characters and vegetation run different vertex
  shaders, so positions only exist after the vertex shader.
- The workable route: transform feedback (`VK_EXT_transform_feedback`) on the opaque scene draws - the translated vertex
  shaders (`runtime/nvn/maxwell/translate.c`) write their clip position to a capture buffer as well, turned back into world
  space by the inverse of the frame's view-projection (as the fireflies do) - then one BLAS built (or refit) per frame from
  those triangles and a TLAS of it; `dl_apply` traces from each lit pixel to its source and keeps the light where nothing is hit.
- Cost and risk: a town frame has ~130-230 opaque draws, roughly 0.5-1.5 M triangles; a full BLAS build of that on a 3060 is
  ~1-3 ms a frame (a refit is cheaper but the draw set changes as the camera moves). The capture doubles the vertex work of those
  draws. Frame generation's re-rendered in-between images would need it too (or reuse N's). Several days: device features,
  the capture path in the shader translator, buffer and AS management, the shader change, then tuning.
- What it would improve: the screen-space march misses occluders off screen or behind the visible surface; with the faint pools
  of this game its measured effect was ~1 / 255 near a lamp. Ray-traced shadows would mostly show as sharper, correct shadows of
  posts, fences and characters inside the lamp pools.

## Frame generation flicker at night, caves, Options rows (2026-10-27)

- Flicker: with frame generation on, a GPU that falls behind gets its in-between images at 50-75 % size (`[fg] auto: ...
  in-between images at 50 %`). The light map was 4x4-pixel blocks of whatever size the image was, so a reduced image's map
  covered twice the scene: larger, brighter pools and halos every other image (+5-6 levels over the lit Center; identical with
  `LARECOMP_LIGHTS=0`, so the lights alone). Now `dl_extract`'s block follows the image's scale (`ip_lo_factor`: 2 at 50 %,
  3 at 75 %) and `dl_apply`'s radii are in full-size pixels (`f.w`). `LARECOMP_FG_SCALE=0.5`, walking past the Center: real
  96.3-96.9, in-between 94.9-95.8 (a 2x2 block of reduced pixels catches a little less peak; was +5-6). `tests/runs/daynight/fgflick/`.
- Caves: Mt. Moon (`tests/saves/mtmoon`: map id = FNV-1a 64 of the pack name `r004d01`, as every map id is; floor 1 bounds from
  its `terrain_data.bin`) loads the `dungeon` light set and looks the same at 12:00 and 22:00 (`results/cave_noon_vs_night.png`).
- The game's own Options list has "Day and night" and "Time of day" (Clock or 00:00-23:00; `--` with day/night off),
  `runtime/hle/pc_settings.c`.

## Ray-traced light shadows: built (2026-10-27)

- `runtime/nvn/vulkan/rt.c`: at night with the dynamic lights on, the scene pass's opaque, depth-writing triangle draws run a
  vertex variant (`ShaderKey.xfb`: `xfb_pos_` = the clip position before zoom / jitter, transform feedback buffer 0) and each is
  captured at its own place of one 48 MB buffer (3 M vertices). After the pass one BLAS is built from it (fast build) with a
  geometry transform from the game's clip space to the lights pass's view space (x / P00, y / P11, (P23 - z) / P22: linear),
  and a one-instance TLAS. `dl_apply_rt.frag` (`dl_apply.frag` with `RT`): the pool's sources' weighted view-space point is kept
  while the pool is summed; 4 ray queries go from the pixel (moved 1.5 % toward the camera, off its surface) to points 60 cm
  around it, stopping 50 cm short; a hit leaves 12 % of the pool. The halo is not shadowed.
- Excluded: blended draws, alpha-cut ones (`uses_discard`: grass tufts, leaves, tree cards - captured whole, their empty corners
  cast square shadows), strips with primitive restart. So trees and bushes cast no ray-traced shadow (alpha testing in the
  ray query would be next). Frame generation's in-between images capture nothing and trace N's structure.
- Note: the screen-space march (`vis()`) was not called any more since the pool rewrite: before this, the pools had no shadows.
- Device: `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, `VK_EXT_transform_feedback`
  and buffer device addresses when all are there (`[vk] ray-traced light shadows: available`); `LARECOMP_RT=0` leaves them off.
- Setting `rt_shadows` (default on): game Options "Ray-traced light shadows" (with day and night), F12 image page;
  `LARECOMP_LIGHTS_RT=0` off for a run. The GLSL translation cache key now includes the variant (it handed back the shaders
  without the capture: an all-zero buffer).
- Checked: `LARECOMP_RT_DEBUG=1` traces from the camera through each pixel - green where the hit matches the depth buffer: the
  ground, paths, lamp posts, buildings and characters match (`tests/runs/daynight/rt1/h22_1900_dbg5_game_01900.png`); it also
  logs a few captured vertices. Viridian at 22:00: ~145-280 K triangles a frame; GPU, frame 1880 at 720p: 4.81 ms with, 3.97
  without (the structures ~1.0 ms). The Center's roof and wall now take a soft shadow where its own roof blocks its windows'
  light (`tests/runs/daynight/rt2/`); a faint fixed 4-ray pattern shows up close.
- Pewter's blocky window halo and green panes: fixed (by the user, 2026-10-27).
- Cloud shapes (2026-10-27): the shadows were rectangles / straight-edged facets. Not the noise type alone: the `fract(sin(dot()))`
  hash lost its precision on the octaves' large cell numbers (GPU `sin` of ~1e5) and its values lined up. Now an integer hash,
  gradient noise with a quintic fade, 5 octaves each turned 37 degrees, and the shape warped by a second fbm: rounded, billowy
  outlines (`tests/runs/daynight/clouds_fbm/h12_1620_i0.6_window.png`, at `LARECOMP_CLOUDS=0.6`).
  Checked: the sun is the game's (`[clouds] sun (-0.39, 0.82, 0.42)`, the shadow map's light, logged once); at normal zoom and
  the 0.4 default the clouds read as soft light and shade, and 7 s apart the patches have drifted (`tests/runs/daynight/clouds_check/`,
  `move_diff.png`); at night (a third of the strength) a faint patch over the grass, lamps and windows untouched
  (`tests/runs/daynight/clouds_night/night_off_on.png`).
- Center door, still breaking up while walking (2026-10-27): not the glass material - by day, and at night with clouds, glow and
  lights off, the door is clean and steady; with the dynamic lights alone it washed out in blocks that moved.
  The lit sign and window band above put their pool and halo on the curved glass, and the quarter-size map's texels
  showed as blocks. Glass (strongly cool, `(b - r) / b` 0.45..0.7, above a low brightness: ratios, the scene being linear HDR - an
  absolute threshold caught a fifth of the door) no longer takes the pool and halo (`dl_apply` `rcv`). Door now one even glass in
  four frames walking (60 fps, TAA, frame generation; the run folders were pruned).
- Also moved: the day / night block runs at the scene's end (`dn_scene_end`: the game switching from the scene target to its
  bloom, or a draw without the depth test onto it - a fade) instead of at the occlusion's hook, so the game's refraction copy
  (the pond, glass) takes the plain scene and those draws are tinted with the rest. Checked after the move: town, menu (the
  background tinted, the menu clean), the Center's interior (untinted), a wild battle at 22:00 (tinted: `tests/runs/daynight/dnbattle/battle3.png`).

## Graphics batch (2026-10-27)

- Atmosphere by hour (`atmo.frag`, `daynight_atmo`): sky multiplied from a horizon to a zenith colour by elevation (pink-gold dawn,
  orange dusk, navy night), stars and a moon at night (not yet seen close up), distance haze by hour. `LARECOMP_ATMO=0` off.
  (Checked at 5:30, 12, 18:30, 22; the run folder was pruned.)
- Sun shadow map at 2x (`textures.c tex_shadow_k`: a square depth target 1024-4096 made K times larger, passes into it take the
  scale): finer edges, small visible gain. `LARECOMP_SHADOW_SCALE=1..4` (default 2).
- Light shafts (`shafts.frag`): the sky as the light, 28 samples toward the sun's place on screen, warm and strongest at dawn / dusk.
  In the top-down views the sun is mostly off screen behind the camera: little effect seen. `LARECOMP_SHAFTS=0` off.
- Water reflections (`ssr.frag`): blue level pixels mirror the view ray (rippled) through the depth buffer; Fresnel, edge fades.
  Visible on the pond's far edge and on wet ground in the rain. `LARECOMP_SSR=0` off, or a strength.
- Ray-traced ambient occlusion (`rtao.frag`): 4 hemisphere rays of 120 cm per pixel through rt.c's structure, which is now built
  once per frame in `ssao_run` for both this and the lights' shadows, day and night. 0.87 ms at 720p. Soft contact under the bench,
  the pond's rim, the planter, the cliff. `LARECOMP_RTAO=0` off, or a strength (default 0.6). Follows the "Ray-traced light
  shadows" setting. `results/rtao_off_on.png`.
- Rain (`rain.frag`, `daynight_rain`): darker, greyer scene, three depth-tested layers of slanted streaks, overcast clouds,
  thicker haze, no stars, wet level ground reflecting (lamps and the Center's windows on the road at night). Setting `weather`
  (Options "Weather": Clear / Rain / Changing - half-hour spells of the real clock, a quarter rainy, a minute's fade);
  `LARECOMP_WEATHER=clear|rain|changing`. `results/rain_noon_night.png`.
- Not done: ray-traced sun shadows and foliage in the ray traces (alpha testing in the ray query: the leaf cards' UVs and textures
  would have to be captured too - days); a moving sun (the game's light direction would have to be patched for its shadow map and
  its shading - research, not started).
- Rain splashes (`rain.frag` `splash`): on level surfaces (the world's up from the depth's derivatives) within 25 m, 35 cm world
  cells each with a drop landing every 0.55-0.9 s at a hashed point - a bright flick, then a ring spreading 14 cm and fading; by
  the scene's light (dim at night). Fixed in the world (this frame's camera). 0.27 ms for the rain pass. `results/rain_splashes.png`.
- Stars: the sky test was "past 200 m", but the game's far plane is 100 m and its sky dome is inside it - only the empty background
  counted. Now past 75 m and not below the horizon; stars a little larger. Seen in a night battle (`results/stars_battle.png`); the
  moon's fixed direction is off screen there.
- Cost with everything on (Viridian 22:00, rain, 60 fps, TAA, frame generation; real frame + in-between): 12.5 ms. Fireflies were the
  dearest (1.37 ms: the edge fix's 4 neighbours on every pixel); now only at depth edges: 0.81 ms. Clouds 0.40, atmosphere 0.06,
  reflections 0.14, rain 0.27, shafts <0.01, ray-traced occlusion 0.89, the acceleration structures ~1 ms.
- More weathers, after weather-canvas's set (MIT; its types, not its code): the setting is Clear / Rain / Changing / Cloudy /
  Thunderstorm / Snow / Fog (`daynight_weather`: rain, snow, fog, cloud, storm amounts; old values kept). Changing: each half hour a
  hashed type (clear 45 %, cloudy 20 %, rain 15 %, fog 8 %, storm 7 %, snow 5 %), a minute's fade at both ends.
  - Cloudy: the clouds' cover and depth up, fewer stars. Rain / storm: overcast. Storm: lightning - in a quarter of 3 s slots, a
    double flicker that lights the scene blue-white (`LARECOMP_FLASH=<0..1>` holds one, for tests).
  - Snow: 3 layers of soft flakes falling and swaying, snow settled on what faces up (broken by a world noise), greyer and dimmer.
  - Fog: the haze 45x denser and pale grey (dark at night: lamps and windows glow through).
  - Shooting stars on clear nights: about every 12 s, 0.8 s across 20 degrees of sky (hidden by clouds and rain, as the stars).
  `results/weather_cloudy_snow_fog_storm.png`, `weather_fog_day_night.png`, `weather_lightning.png`.
- Settings for the graphics (game Options and F12): Cloud shadows (percent, default 40), Sky and haze, Water reflections, Light
  shafts (all with day and night), Ray-traced occlusion (its own switch now, not the light shadows'). The variables still override.
- Snow freezes the water: strongly blue level pixels to a pale icy blue at their brightest channel (`results/frozen_pond.png`).
- Stars hidden fully under overcast, rain and snow. Lamp-shadow rays turned each frame (TAA averages their pattern). The temporary
  ray-tracing debug readback in rt.c removed.
- Checked: the Options rows (Weather to Cloudy, Cloud shadows 40 -> 30, saved); storm in the Center and snow in Mt. Moon - both
  clear; snow in a night battle; the Mart door walking at 60 fps / TAA / frame generation - steady (the doorway is bright: the
  game's bloom on its pale glass); the game's sun does not move with the hour, so the clouds' projection never goes low.

## Moving sun (2026-10-27)

- The game's sun is fixed: `field/draw_env/default/EnvShadowLight.gfblt` (the shadow map's light, (0.395, -0.819, -0.415)) and
  `EnvDirLight0-2.gfblt` (the shading's), a float3 at byte 40. `fs.c` notes where each loads (`note_sun`); the game reads them
  once and keeps its own copies (found 2 MB around each table, `sun_find`: the shadow light ~30 KB away, the shading's 3), and
  writes per-frame constant buffers from those. `daynight_sun_update` (each outdoor frame) turns every direction: azimuth
  +-70 degrees from 6:00 to 18:00 (east to west), height from 40 % of the game's at the ends to all of it at noon; night holds the
  evening's. Writes are checked: a place whose value is neither the original nor what was written is left alone (reused memory).
- Setting `moving_sun` (Options "Moving sun", default on, with day and night); `LARECOMP_MOVING_SUN=0/1`; research:
  `LARECOMP_SUN_TEST=x,y,z` (into the tables at load), `LARECOMP_SUN_SCAN=1` (heap search), `LARECOMP_SUN_SKIP=<bits>`.
- `results/moving_sun_8_12_16.png`: at 8:00 the lamp's shadow falls back and left, at noon the game's short shadows, at 16:00 long
  shadows to the right; the shading turns with it. The clouds follow (they read the shadow map's light).
- Regression found on the way: the 2x sun shadow map (`LARECOMP_SHADOW_SCALE`) removed every sun shadow - default back to 1
  (the game's), and a comment that had swallowed the end of `tex_shadow_k` (it returned garbage after the first call) fixed.

## Performance pass (2026-10-27)

- Report: 60 fps -> ~50 and laggy with the new effects (TAA, frame generation Auto, 1080p+). With frame generation a real frame and its
  in-between image share 16.7 ms; measured ~17.5 ms, so Auto lowered the in-between images.
- Clouds were the dearest (1.1 ms at 1080p, and again in each in-between image): a 4x4 inverse per pixel and 25 noise lookups. The
  inverse is now made once on the CPU (passed instead of the matrix), the octaves 2 + 2 (warp), 4 (shape), 3 (wisps) = 11 lookups,
  the distance from the depth: 0.48 ms. Atmosphere (3 inverses per pixel) and rain take the inverse too (atmosphere 0.16 -> 0.09 ms);
  fireflies make theirs once per pixel instead of up to 5 times. The frame measured 13.7 ms.
- The moving sun's search for the game's copies (16 MB read) retried every frame for up to 120 frames after a load; now every
  15 frames, 8 times, comparing one word first.
