"""Generates the mushrooms the ground cover places (assets/plants/mushrooms): a glTF with a few variants (a porcini, a
fly agaric, a pair of them, a cluster of brown honey fungus, a cluster of small pale ones) and their painted texture
atlas, in the layout GroundCover::init loads (opaque: <name>_diff_1k.jpg + <name>_nor_gl_1k.jpg).

    python tools/make_mushrooms.py

The atlas (1024^2): brown cap, red cap with white spots, pale cap (top row / bottom left), stem, gills.
Units are metres, y up, every variant at its own origin (the loader ignores node translations)."""
import json, math, os, random, struct

from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(__file__), '..', 'assets', 'plants', 'mushrooms')
SIZE = 1024

# atlas rects (u0, v0, u1, v1), uv space (v down, as glTF)
CAP_BROWN = (0.0, 0.0, 0.5, 0.5)
CAP_RED = (0.5, 0.0, 1.0, 0.5)
CAP_PALE = (0.0, 0.5, 0.5, 1.0)
STEM = (0.5, 0.5, 0.75, 1.0)
GILLS = (0.75, 0.5, 1.0, 1.0)


def lerp(a, b, t):
    return a + (b - a) * t


class Mesh:
    def __init__(self):
        self.pos, self.nor, self.uv, self.idx = [], [], [], []

    def vertex(self, p, n, uv):
        self.pos.append(p)
        l = math.sqrt(sum(c * c for c in n)) or 1.0
        self.nor.append(tuple(c / l for c in n))
        self.uv.append(uv)
        return len(self.pos) - 1

    def grid(self, rows, cols, fn):
        # fn(i, j) -> (pos, normal, uv) for i in 0..rows, j in 0..cols (j wraps around)
        base = len(self.pos)
        for i in range(rows + 1):
            for j in range(cols + 1):
                self.vertex(*fn(i, j))
        w = cols + 1
        for i in range(rows):
            for j in range(cols):
                a, b, c, d = base + i * w + j, base + i * w + j + 1, base + (i + 1) * w + j, base + (i + 1) * w + j + 1
                self.idx += [a, c, b, b, c, d]


def mushroom(mesh, at, cap_r, cap_h, stem_h, stem_r, cap_rect, lean=(0.0, 0.0), flat=0.0, bulb=0.0, seed=0):
    """one mushroom: a domed cap (flat: 0 round .. 1 flat topped), the gills under it, a stem (bulb: a thicker foot)"""
    rnd = random.Random(seed)
    ox, oz = at
    lx, lz = lean
    seg = 16
    phi_max = math.radians(lerp(105.0, 80.0, flat))
    top = stem_h

    def stem_point(t, ang, r):
        # the stem leans: its top moved by lean * height
        return (ox + lx * t * stem_h + math.cos(ang) * r, t * stem_h, oz + lz * t * stem_h + math.sin(ang) * r)

    cx, cz = ox + lx * stem_h, oz + lz * stem_h
    wobble = [1.0 + (rnd.random() - 0.5) * 0.12 for _ in range(seg)]

    def cap(i, j):
        phi = phi_max * i / 8.0
        ang = 2.0 * math.pi * j / seg
        w = wobble[j % seg]
        r = cap_r * math.sin(phi) * w
        y = top + cap_h * (math.cos(phi) - math.cos(phi_max)) / (1.0 - math.cos(phi_max))
        if flat > 0.0:  # flatter top: pull the upper half down towards a plate
            y = lerp(y, top + cap_h * 0.75, flat * (1.0 - phi / phi_max) * 0.5)
        p = (cx + math.cos(ang) * r, y, cz + math.sin(ang) * r)
        n = (math.cos(ang) * math.sin(phi), math.cos(phi) * (cap_r / max(cap_h, 1e-4)) ** 0.5, math.sin(ang) * math.sin(phi))
        k = 0.47 * (phi / phi_max)  # planar from the top, the rim at the rect's edge
        u = lerp(cap_rect[0], cap_rect[2], 0.5 + math.cos(ang) * k)
        v = lerp(cap_rect[1], cap_rect[3], 0.5 + math.sin(ang) * k)
        return p, n, (u, v)
    mesh.grid(8, seg, cap)

    rim_y = top + cap_h * 0.0

    def gills(i, j):
        t = i / 2.0
        ang = 2.0 * math.pi * j / seg
        r = lerp(cap_r * math.sin(phi_max) * wobble[j % seg] * 0.98, stem_r * 1.1, t)
        y = lerp(rim_y, top + cap_h * 0.25, t)
        p = (cx + math.cos(ang) * r, y, cz + math.sin(ang) * r)
        k = lerp(0.47, 0.08, t)
        u = lerp(GILLS[0], GILLS[2], 0.5 + math.cos(ang) * k)
        v = lerp(GILLS[1], GILLS[3], 0.5 + math.sin(ang) * k)
        return p, (0.0, -1.0, 0.0), (u, v)
    mesh.grid(2, seg, gills)

    def stem(i, j):
        t = i / 4.0
        ang = 2.0 * math.pi * j / seg
        r = stem_r * (1.0 + bulb * (1.0 - t) ** 2) * lerp(1.0, 0.85, t)
        p = stem_point(t * 1.02, ang, r)
        u = lerp(STEM[0], STEM[2], j / seg)
        v = lerp(STEM[3], STEM[1], t)
        return p, (math.cos(ang), 0.15, math.sin(ang)), (u, v)
    mesh.grid(4, seg, stem)


def variants():
    out = []
    m = Mesh()  # a porcini: brown, fat stem with a bulb
    mushroom(m, (0, 0), 0.065, 0.045, 0.07, 0.022, CAP_BROWN, flat=0.2, bulb=0.6, seed=1)
    out.append(('porcini', m))
    m = Mesh()  # a fly agaric
    mushroom(m, (0, 0), 0.07, 0.03, 0.14, 0.012, CAP_RED, flat=0.6, bulb=0.5, seed=2)
    out.append(('fly_agaric', m))
    m = Mesh()  # a pair of fly agarics, one young (round)
    mushroom(m, (0, 0), 0.06, 0.028, 0.12, 0.011, CAP_RED, lean=(0.05, 0.0), flat=0.6, bulb=0.5, seed=3)
    mushroom(m, (0.06, 0.04), 0.035, 0.03, 0.06, 0.009, CAP_RED, lean=(-0.05, 0.05), flat=0.0, bulb=0.4, seed=4)
    out.append(('fly_agaric_pair', m))
    rnd = random.Random(7)
    m = Mesh()  # honey fungus: a clump of thin brown ones leaning out
    for k in range(6):
        a = k / 6.0 * 2 * math.pi + rnd.random() * 0.6
        d = 0.015 + rnd.random() * 0.02
        h = 0.05 + rnd.random() * 0.05
        mushroom(m, (math.cos(a) * d, math.sin(a) * d), 0.022 + rnd.random() * 0.012, 0.012, h, 0.004,
                 CAP_BROWN, lean=(math.cos(a) * 0.25, math.sin(a) * 0.25), flat=0.5, seed=10 + k)
    out.append(('honey_cluster', m))
    m = Mesh()  # small pale ones scattered
    for k in range(5):
        a = rnd.random() * 2 * math.pi
        d = rnd.random() * 0.07
        mushroom(m, (math.cos(a) * d, math.sin(a) * d), 0.012 + rnd.random() * 0.01, 0.012, 0.025 + rnd.random() * 0.025,
                 0.0028, CAP_PALE, lean=((rnd.random() - 0.5) * 0.2, (rnd.random() - 0.5) * 0.2), flat=0.1, seed=20 + k)
    out.append(('pale_cluster', m))
    return out


def paint_atlas():
    img = Image.new('RGB', (SIZE, SIZE), (200, 190, 170))
    d = ImageDraw.Draw(img)
    rnd = random.Random(42)
    half = SIZE // 2

    def cap(x0, y0, centre, rim, spots=None):
        # radial gradient from the centre colour to the rim's, mottled
        for r in range(half // 2, 0, -1):
            t = r / (half / 2)
            c = tuple(int(lerp(centre[i], rim[i], t ** 1.5)) for i in range(3))
            d.ellipse([x0 + half / 2 - r, y0 + half / 2 - r, x0 + half / 2 + r, y0 + half / 2 + r], fill=c)
        for _ in range(900):
            x, y = x0 + rnd.random() * half, y0 + rnd.random() * half
            s = rnd.random() * 6 + 2
            k = rnd.uniform(0.85, 1.12)
            px = img.getpixel((int(min(x, SIZE - 1)), int(min(y, SIZE - 1))))
            d.ellipse([x - s, y - s, x + s, y + s], fill=tuple(min(255, int(c * k)) for c in px))
        if spots:
            for _ in range(38):
                a = rnd.random() * 2 * math.pi
                rr = math.sqrt(rnd.random()) * half * 0.44
                x, y = x0 + half / 2 + math.cos(a) * rr, y0 + half / 2 + math.sin(a) * rr
                s = rnd.uniform(6, 16)
                d.ellipse([x - s, y - s * 0.8, x + s, y + s * 0.8], fill=spots)

    cap(0, 0, (96, 62, 34), (150, 104, 60))                        # porcini / honey fungus brown
    cap(half, 0, (190, 30, 18), (225, 70, 30), spots=(240, 236, 222))  # fly agaric
    cap(0, half, (226, 214, 190), (206, 190, 160))                 # pale
    # stem: cream with fibres along it
    x0, x1 = half, half + half // 2
    d.rectangle([x0, half, x1, SIZE], fill=(226, 218, 200))
    for _ in range(400):
        x = rnd.uniform(x0, x1)
        y = rnd.uniform(half, SIZE)
        d.line([x, y, x + rnd.uniform(-2, 2), y + rnd.uniform(20, 60)], fill=tuple(int(c * rnd.uniform(0.88, 1.0)) for c in (226, 218, 200)), width=2)
    # gills: beige with radial lines from the centre of their square
    gx, gy = half + half // 2, half
    d.rectangle([gx, gy, SIZE, SIZE], fill=(214, 198, 168))
    cx, cy = gx + half // 4, gy + half // 2
    for k in range(180):
        a = k / 180 * 2 * math.pi
        d.line([cx, cy, cx + math.cos(a) * half, cy + math.sin(a) * half], fill=(170, 150, 120), width=2)
    return img.filter(ImageFilter.GaussianBlur(1.2))


def write_gltf(meshes):
    os.makedirs(os.path.join(OUT, 'textures'), exist_ok=True)
    blob = bytearray()
    views, accessors, gl_meshes, nodes = [], [], [], []

    def add(data, fmt, count, kind, target, comp, mins=None, maxs=None):
        while len(blob) % 4: blob.append(0)
        off = len(blob)
        blob.extend(struct.pack('<' + fmt * count, *data) if fmt != 'f' or True else b'')
        views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(blob) - off, 'target': target})
        acc = {'bufferView': len(views) - 1, 'componentType': comp, 'count': count // {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[kind], 'type': kind}
        if mins is not None: acc['min'], acc['max'] = mins, maxs
        accessors.append(acc)
        return len(accessors) - 1

    for name, m in meshes:
        flat = [c for p in m.pos for c in p]
        mins = [min(p[i] for p in m.pos) for i in range(3)]
        maxs = [max(p[i] for p in m.pos) for i in range(3)]
        ip = add(flat, 'f', len(flat), 'VEC3', 34962, 5126, mins, maxs)
        inn = add([c for n in m.nor for c in n], 'f', len(m.nor) * 3, 'VEC3', 34962, 5126)
        iuv = add([c for t in m.uv for c in t], 'f', len(m.uv) * 2, 'VEC2', 34962, 5126)
        ii = add(m.idx, 'I', len(m.idx), 'SCALAR', 34963, 5125)
        gl_meshes.append({'name': name, 'primitives': [{'attributes': {'POSITION': ip, 'NORMAL': inn, 'TEXCOORD_0': iuv}, 'indices': ii, 'material': 0}]})
        nodes.append({'mesh': len(gl_meshes) - 1, 'name': name})

    with open(os.path.join(OUT, 'mushrooms.bin'), 'wb') as f:
        f.write(blob)
    gltf = {
        'asset': {'version': '2.0', 'generator': 'tools/make_mushrooms.py'},
        'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}], 'nodes': nodes, 'meshes': gl_meshes,
        'materials': [{'name': 'mushrooms', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0}}],
        'textures': [{'source': 0}], 'images': [{'uri': 'textures/mushrooms_diff_1k.jpg'}],
        'buffers': [{'uri': 'mushrooms.bin', 'byteLength': len(blob)}], 'bufferViews': views, 'accessors': accessors,
    }
    with open(os.path.join(OUT, 'mushrooms_1k.gltf'), 'w') as f:
        json.dump(gltf, f, indent=1)


if __name__ == '__main__':
    ms = variants()
    write_gltf(ms)
    paint_atlas().save(os.path.join(OUT, 'textures', 'mushrooms_diff_1k.jpg'), quality=92)
    Image.new('RGB', (64, 64), (128, 128, 255)).save(os.path.join(OUT, 'textures', 'mushrooms_nor_gl_1k.jpg'), quality=95)  # flat
    for name, m in ms:
        print(name, len(m.idx) // 3, 'triangles')
