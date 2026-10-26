# Agent instructions

## Tests, logs and captures live in `tests/`

`tests/` is ignored by Git (see `tests/README.md`). Put everything a run produces there, never in the system temp
folder, the scratchpad or the repo tree:

- **Runs** (logs, screenshots, window captures, per-run settings and save copies): one folder per run,
  `tests/runs/daynight/<run>/` or `tests/runs/<feature><N>/`. Only the 2 newest runs of each feature are kept:
  `tests/scripts/prune_runs.py` deletes the rest and the run scripts call it. Images worth keeping go in
  `tests/runs/daynight/results/` (never pruned).
- **Build and tool logs**: `tests/logs/`.
- **Scripts** that start runs: `tests/scripts/` (e.g. `run-daynight.sh <hour>`). Prefer extending these over one-off commands.
- **Moving the player** (to test a town): `tests/scripts/save_pos.py` edits a save copy's position (see `docs/DAYNIGHT.md`);
  `tests/saves/viridian-lamps/` is ready-made. Town / road extents: `tests/logs/area-positions.txt`.
- **Save copies**: scripted runs always use a copy of `build/save` inside `tests/runs/<feature>/` (or `tests/saves/`);
  never touch the player's own save or `build/larecomp.ini`.
- After a test, add the new logs and images to that folder and name the files in your report.

## Building and running (Windows)

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -j16 build/LetsGoRecomp.exe        # the runtime only; the game code (build/extracted/lasgame.dll) is already built
cd build && ./LetsGoRecomp.exe extracted --play
```

Screenshots: `LARECOMP_WINDOW_SHOT=<frame>` is the image that reaches the screen (post passes included);
`LARECOMP_SHOT_AT` is the game image before them.
