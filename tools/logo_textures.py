"""logo_textures.py <logo.psd> <logo.png> <romfs.bin> <out dir>: our title logo in place of the game's (runtime/ui2x.c).

The title screen draws its logo from three textures of bin/app/title/bin/title_{p,v}_00*.arc: title_logo_00 ("Pokemon"
alone), title_logo_{p,v}_01 (the "Let's Go" part alone, = the bottom of _00 from row 80) and title_logo_{p,v}_00 (both).
The PSD's groups give the pieces: 'Layer 3' (Pokemon, on top) and 'Lets Go Online'. Their pixels come from logo.png
(Photoshop's own render: psd-tools draws no drop shadow and boxes the Pokeball's stroke); psd-tools' render of each
group is its mask, and gives the "Let's Go" pixels Pokemon covers. Each is placed as the originals are (2x their size),
then <out dir>/textures.txt lists, for every language's key of those textures, "<key> <w> <h> <png>".
Needs psd-tools, aggdraw, scipy, pillow, numpy.
"""
import os
import sys

import numpy as np
from PIL import Image
from psd_tools import PSDImage
from scipy import ndimage as nd

sys.path.insert(0, os.path.dirname(__file__))
import ui_texkeys as T

K = 2                      # the replacements' scale over the originals
P00 = (830, 400)           # title_logo_{p,v}_00
P01_ROW = 80               # title_logo_{p,v}_01: _00 from this row, 290 rows
L00, L00_AT = (800, 214), (15, -3)   # title_logo_00: _00 seen from (15, -3)
PNG = {'title_logo_00': 'title_logo_00.png', 'title_logo_p_00': 'title_logo_both.png', 'title_logo_p_01': 'title_logo_letsgo.png'}
BOX = (4, 0, 825, 359)     # where the English _00 has its logo


def pieces(psd_path, png_path):
    psd = PSDImage.open(psd_path)
    ref = np.array(Image.open(png_path).convert('RGBA')).astype(np.float32)
    logo = psd[0]
    group = {l.name: np.array(l.composite(viewport=psd.viewbox, force=True).convert('RGBA')).astype(np.float32) for l in logo}
    on, pk = group['Lets Go Online'], group['Layer 3']
    assert ref.shape == on.shape, 'logo.png is not the PSD canvas: export it again'
    a_on, a_pk = on[..., 3], pk[..., 3]

    def shadow_zone(a):  # (the Logo group's drop shadow: 5 px down, 5 px soft)
        return nd.binary_dilation(np.roll(a > 0, 5, axis=0), iterations=6)

    def cut(alpha, rgb):
        out = np.dstack([rgb, alpha])
        out[alpha <= 0] = 0
        return out

    # Pokemon: on top, so logo.png has it all; its shadow only where "Let's Go" isn't
    alpha = np.where(a_on > 0, a_pk, np.maximum(a_pk, ref[..., 3] * shadow_zone(a_pk)))
    pokemon = cut(alpha, ref[..., :3])
    # "Let's Go": logo.png except under Pokemon, there psd-tools' own render
    under = (a_pk > 0) & (a_on > 0)
    alpha = np.where(under, a_on, np.where(a_pk > 0, 0, ref[..., 3] * ((a_on > 0) | shadow_zone(a_on))))
    letsgo = cut(alpha, np.where(under[..., None], on[..., :3], ref[..., :3]))
    return ref, pokemon, letsgo


def place(img, bbox, size, at):
    """img's bbox scaled into BOX of a _00-sized frame, seen from at, cut to size (all at K x)."""
    x0, y0, x1, y1 = bbox
    s = K * min((BOX[2] - BOX[0]) / (x1 - x0), (BOX[3] - BOX[1]) / (y1 - y0))
    w, h = round((x1 - x0) * s), round((y1 - y0) * s)
    im = Image.fromarray(img.clip(0, 255).astype(np.uint8), 'RGBA').crop(bbox)
    # (premultiplied while scaled: no dark fringes)
    im = im.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')
    out = Image.new('RGBA', (size[0] * K, size[1] * K))
    ox = K * BOX[0] + (K * (BOX[2] - BOX[0]) - w) // 2 - K * at[0]
    oy = K * BOX[1] + (K * (BOX[3] - BOX[1]) - h) // 2 - K * at[1]
    out.paste(im, (ox, oy))  # (onto an empty frame: paste = composite, and it takes negative offsets)
    return out


def main():
    psd_path, png_path, rom_path, out_dir = sys.argv[1:5]
    ref, pokemon, letsgo = pieces(psd_path, png_path)
    ys, xs = np.nonzero(ref[..., 3] > 0)
    bbox = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    made = {'title_logo_00.png': place(pokemon, bbox, L00, L00_AT),
            'title_logo_both.png': place(ref, bbox, P00, (0, 0)),
            'title_logo_letsgo.png': place(letsgo, bbox, (P00[0], 290), (0, P01_ROW))}
    os.makedirs(out_dir, exist_ok=True)
    for name, im in made.items():
        im.save(os.path.join(out_dir, name), optimize=True)
    rows = set()
    rom = open(rom_path, 'rb')
    for path, size, off in T.romfs_files(rom):
        if not (path.startswith('bin/app/title/bin/title_') and path.endswith('.arc')):
            continue
        rom.seek(off)
        for _, fd in T.sarc_files(rom.read(size)):
            if fd[:4] != b'BNTX':
                continue
            for name, w, h, _, key in T.bntx_textures(fd):
                name = name.split('^')[0]
                png = {'title_logo_00': 'title_logo_00.png'}.get(name) or \
                    {'00': 'title_logo_both.png', '01': 'title_logo_letsgo.png'}.get(name[-2:]) if name.startswith('title_logo') else None
                if png:
                    rows.add(f'{key} {w} {h} {png}')
    assert len(rows) >= 3, 'no title logo textures found'
    with open(os.path.join(out_dir, 'textures.txt'), 'w', newline='\n') as f:
        f.write('# <content key> <original w> <original h> <png>: made by tools/logo_textures.py (runtime/ui2x.c)\n')
        f.write(''.join(sorted(r + '\n' for r in rows)))
    print(f'{len(made)} images, {len(rows)} keys -> {out_dir}')


if __name__ == '__main__':
    main()
