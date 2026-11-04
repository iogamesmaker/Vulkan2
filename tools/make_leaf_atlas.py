"""Builds the tree leaf card atlases from the branch photos in assets/branches (run from the repo root:
python tools/make_leaf_atlas.py).

assets/leaves_conifer.png  (1024x512): left = spruce branch (28), right = pine branch (29), turned so the branch
                           grows along u (left = base, right = tip), like the conifer cards in trees.cpp
assets/leaves_broadleaf.png (1024x512): left = green oak branch (39), right = autumn maple branch (26), upright
                           (base at the bottom centre) for the centred cluster cards

transparent texels take the colour of the nearest leaves, so the mips don't get dark fringes.
the branch photos: Yughues, "Branches textures pack 02 / 04", OpenGameArt, CC0 (see assets/branches/README.txt)
"""
import numpy as np
from PIL import Image, ImageFilter

TILE = 512


def bleed(img):
    a = np.asarray(img).astype(np.float32)
    rgb, alpha = a[..., :3], a[..., 3] / 255.0
    solid = alpha >= 0.5
    out = rgb.copy()
    filled = solid.copy()
    for radius in (1, 2, 4, 8, 16, 32, 64):
        weight = Image.fromarray((filled * 255).astype(np.uint8)).filter(ImageFilter.BoxBlur(radius))
        w = np.asarray(weight).astype(np.float32) / 255.0
        acc = [np.asarray(Image.fromarray(np.clip(out[..., i] * filled, 0, 255).astype(np.uint8)).filter(ImageFilter.BoxBlur(radius))).astype(np.float32) for i in range(3)]
        fill = np.stack(acc, -1) / np.maximum(w[..., None], 1e-4)
        new = (~filled) & (w > 0.02)
        out[new] = fill[new]
        filled |= new
    a[..., :3] = out
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


def tile(path, rotate=0):
    im = Image.open(path).convert('RGBA')
    if rotate: im = im.rotate(rotate, expand=True)
    return im.resize((TILE, TILE), Image.LANCZOS)


def atlas(left, right, out):
    image = Image.new('RGBA', (TILE * 2, TILE), (0, 0, 0, 0))
    image.paste(left, (0, 0))
    image.paste(right, (TILE, 0))
    bleed(image).save(out)


if __name__ == '__main__':
    # rotate -90 = clockwise: the branch base (bottom of the photo) ends up on the left
    atlas(tile('assets/branches/spruce.png', -90), tile('assets/branches/pine.png', -90), 'assets/leaves_conifer.png')
    atlas(tile('assets/branches/oak.png'), tile('assets/branches/maple_autumn.png'), 'assets/leaves_broadleaf.png')
    print('wrote assets/leaves_conifer.png, assets/leaves_broadleaf.png')
