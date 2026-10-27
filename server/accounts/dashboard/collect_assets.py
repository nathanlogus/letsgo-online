"""Collects the dashboard's artwork into one folder, mounted read-only into the API container (ACCOUNT_DASHBOARD_ASSETS).
The game's textures stay out of git and out of the image: they come from a UI export of the player's own game files
(tests/runs/ui-assets-export-*), the official Let's Go website's artwork (downloaded) and the repository's assets/
folder (logo, emotes). Pokemon images (c) Nintendo, Creatures, GAME FREAK, The Pokemon Company; the dashboard shows
that notice. Never commit the collected folder.

  python server/accounts/dashboard/collect_assets.py <ui export dir> <repo assets dir> <out dir>
"""
import csv
import re
import shutil
import sys
import urllib.request
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
    "mystery_tag_00^q": "tag.png", "mystery_btn_base_01^q": "button.png",
    "mystery_monbo_01^s": "ball.png", "mystery_bag_00^s": "bag.png",
}
GENDER = "snmf"  # the Pokemon icon of a species/form: the shared one first, then genderless, male, female
# The official site's artwork (https://pokemonletsgo.pokemon.com/en-us/), same copyright as above.
SITE_BASE = "https://pokemonletsgo.pokemon.com/assets/img/"
SITE = {
    "home/header-bg.jpg": "site-meadow.jpg", "home/story-bg.jpg": "site-sky.jpg",
    "common/pattern-pokeball.png": "site-pattern-ball.png", "common/pattern-pokemon.png": "site-pattern-pokemon.png",
    "en-us/logo-pokemon-letsgo-pikachu.png": "site-logo-pikachu.png", "en-us/logo-pokemon-letsgo-eevee.png": "site-logo-eevee.png",
    "common/char-pikachu.png": "site-pikachu.png", "common/char-eevee.png": "site-eevee.png",
    "home/story-pikachu.png": "site-trainer-pikachu.png", "home/story-eevee.png": "site-trainer-eevee.png",
    "footer/tail-pikachu.png": "site-tail-pikachu.png", "footer/tail-eevee.png": "site-tail-eevee.png",
}
REPO = {"logo.png": "logo.png", "emotes/joyful-mark.png": "joyful.png", "emotes/teary-mark.png": "teary.png"}


def crop(source, target):
    """A Pokemon/item icon cut to its sprite (the game's icons sit in a larger transparent canvas), so the page can show it big."""
    try:
        from PIL import Image
    except ImportError:  # (no Pillow: the whole canvas)
        shutil.copyfile(source, target); return
    image = Image.open(source).convert("RGBA")
    box = image.getbbox()
    if box:
        image = image.crop((max(0, box[0] - 1), max(0, box[1] - 1), min(image.width, box[2] + 1), min(image.height, box[3] + 1)))
    image.save(target)


def main(export, assets, out):
    out.mkdir(parents=True, exist_ok=True)
    found, icons = {}, {}
    with (export / "manifest.csv").open(encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            path = export / (row["2x_mask"] or row["2x"] or row["original"])  # (masks: white, alpha)
            if row["group"] == "mystery" and row["name"] in PICK and row["name"] not in found:
                found[row["name"]] = path
            # poke_icon_<species>_<form><gender>_n (n: the normal colours), item_<id>
            if m := re.fullmatch(r"poke_icon_(\d{3})_(\d\d)([snmf])_n\^\w", row["name"]):
                key, rank = f"pk-{int(m[1])}-{int(m[2])}.png", GENDER.index(m[3])
                if key not in icons or rank < icons[key][0]:
                    icons[key] = (rank, path)
            elif m := re.fullmatch(r"item_(\d{4})\^\w", row["name"]):
                icons[f"it-{int(m[1])}.png"] = (0, path)
    missing = sorted(set(PICK) - set(found))
    if missing:
        raise SystemExit("Not in the export: " + ", ".join(missing))
    for name, source in found.items():
        shutil.copyfile(source, out / PICK[name])
    for name, (_, source) in icons.items():
        crop(source, out / name)
    for source, name in REPO.items():
        shutil.copyfile(assets / source, out / name)
    shutil.copyfile(assets.parent / "docs/icon/icon.ico", out / "favicon.ico")  # (the Keycloak theme's favicon: img/favicon.ico)
    for source, name in SITE.items():
        if not (out / name).exists():  # (downloaded once)
            with urllib.request.urlopen(SITE_BASE + source, timeout=30) as r:
                (out / name).write_bytes(r.read())
    (out / "NOTICE.txt").write_text("Pokemon images (c) Nintendo, Creatures, GAME FREAK, The Pokemon Company. From a UI export of the game "
                                    "and https://pokemonletsgo.pokemon.com/. For this private dashboard only; do not commit or redistribute.\n")
    print(f"{len(found) + len(REPO) + len(icons) + len(SITE)} files in {out}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*(Path(a) for a in sys.argv[1:]))
