"""Collects the dashboard's artwork into one folder, mounted read-only into the API container (ACCOUNT_DASHBOARD_ASSETS).
The game's textures stay out of git and out of the image: they come from a UI export of the player's own game files
(tests/runs/ui-assets-export-*) and the repository's assets/ folder (logo, emotes).

  python server/accounts/dashboard/collect_assets.py <ui export dir> <repo assets dir> <out dir>
"""
import csv
import shutil
import sys
from pathlib import Path

# export name -> dashboard file (the 2x texture when the export has one; a single-channel mask as white with alpha)
PICK = {
    "mystery_bg_00^o": "bg.png",
    "mystery_frame_00^q": "frame.png",
    "mystery_present_00^s": "present.png",
    "mystery_btn_mark_00^u": "tab-0.png", "mystery_btn_mark_01^u": "tab-1.png",
    "mystery_btn_mark_02^u": "tab-2.png", "mystery_btn_mark_03^u": "tab-3.png",
    "mystery_btn_mark_sel_00^u": "tab-0-on.png", "mystery_btn_mark_sel_01^u": "tab-1-on.png",
    "mystery_btn_mark_sel_02^u": "tab-2-on.png", "mystery_btn_mark_sel_03^u": "tab-3-on.png",
    "pber_pika_00^s": "pikachu.png", "pber_vee_00^s": "eevee.png",
}
REPO = {"logo.png": "logo.png", "emotes/joyful-mark.png": "joyful.png", "emotes/teary-mark.png": "teary.png"}


def main(export, assets, out):
    out.mkdir(parents=True, exist_ok=True)
    found = {}
    with (export / "manifest.csv").open(encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            if row["group"] == "mystery" and row["name"] in PICK and row["name"] not in found:
                found[row["name"]] = export / (row["2x_mask"] or row["2x"] or row["original"])  # (masks: white, alpha)
    missing = sorted(set(PICK) - set(found))
    if missing:
        raise SystemExit("Not in the export: " + ", ".join(missing))
    for name, source in found.items():
        shutil.copyfile(source, out / PICK[name])
    for source, name in REPO.items():
        shutil.copyfile(assets / source, out / name)
    print(f"{len(found) + len(REPO)} files in {out}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*(Path(a) for a in sys.argv[1:]))
