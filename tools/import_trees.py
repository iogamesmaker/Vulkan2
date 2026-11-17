"""Imports the tree packs (Sketchfab glTF exports) into the engine's own format (run from the repo root):

    python tools/import_trees.py <folder with one extracted pack per subfolder: oak, birch, maple, pine, fir>
    python tools/import_trees.py --shrubs     (only re-imports SHRUB_PACKS, keeps the trees already in trees.bin)

the shrub packs (SHRUB_PACKS: lilacs, big bushes, cacti) used to be ground cover: their downloads are in
assets/incoming (tools/import_packs.py's), they're imported the same way as the trees and placed by trees_place.comp
where the ground cover's own pick (groundcoverpick.glsl) chooses them, so they get the trees' LODs, impostors and
shadows. --shrubs needs no tree downloads: it reads the trees back from trees.bin and the generated tables

every pack holds several trees, each a group node with LOD0 / LOD1 / LOD2 children (and a billboard LOD3, skipped:
the engine bakes its own impostors) laid out side by side. per tree and LOD this keeps the bark and the leaf
cluster primitives, drops the layout offsets, stands the tree on its trunk at the origin and writes:

  assets/trees/trees.bin            every mesh's vertices (Trees::TreeVertex) + indices, the LOD table
  assets/trees/<pack>/bark*.png     the bark's colour / normal map (at most 2048 texels on a side)
  assets/trees/<pack>/leaves*.png   the leaf cluster atlas (colour + coverage, bled into the gaps so the mips don't
                                    get dark fringes) / its normal map
  src/treemeshes.h, src/shaders/treemeshes.glsl
                                    the generated tables: the packs, every mesh's size and trunk, which meshes are
                                    young trees (placement, collision, the impostor atlas)
  assets/trees/README.md            the credits (the packs are CC-BY)

tree space: the tree stands at the origin, y up, scaled so the engine's "size" (TreeInstance posScale.w) is 1: the
tree fits into the impostor card (height <= 1.08, radius <= 0.58 of the size, the card covers -0.6..0.6 around 0.5)
"""
import json, os, re, struct, sys
import numpy as np
from PIL import Image, ImageFilter

# the packs, in the order the engine indexes them: folder, group (0 conifer, 1 broadleaf), display name, scale
# (the fir pack is modelled at about half size: its "large" firs were 9m tall, real ones are 15-30m)
PACKS = [
    ("pine", 0, "Pine", 1.0),
    ("fir", 0, "Fir", 2.0),
    ("oak", 1, "Oak", 1.0),
    ("maple", 1, "Maple", 1.0),
    ("birch", 1, "Birch", 1.0),
]
# the shrubs and small trees that were ground cover: assets/incoming folder, engine folder, display name, scale,
# [(kind, plant group regex)] (each kind its own mesh range, SHRUB_FIRST / SHRUB_COUNT), rigid (no sway: the cacti)
SHRUB_PACKS = [
    ("lilac_bush_pack_12_vars_lods_game_ready", "lilac", "Lilac", 1.0,
     [("LILAC_TREE", r"^Lilac_single_tree_\d+$"), ("LILAC_BUSH", r"^Lilac_(bush|small_bush)_\d+$"), ("LILAC_SAPLING", r"^Lilac_sapling_\d+$")], False),
    ("bush_models_pack_gameready_lods", "bush", "Bush", 1.0, [("BUSH_BIG", r"^Bush_big_\d+$")], False),
    ("cactus_pack_18_var_game_ready_lod_realistic", "cactus", "Cactus", 1.0, [("CACTUS", r"^Single_cactus_")], True),
]
PACK_GROUP_SHRUB = 2
LODS = 3
YOUNG_HEIGHT = 7.0       # metres: shorter trees are the young ones (saplings, the forest's edge)
METERS_PER_UNIT = 6.0    # Camera::metersPerUnit
MAX_TEXTURE = 2048
SKIP_GROUPS = ("ground", "man ref", "seasons", "example", "back", "ref_plane", "checker")

COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
DTYPES = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}


class Gltf:
    def __init__(self, folder):
        self.folder = folder
        self.json = json.load(open(os.path.join(folder, "scene.gltf"), encoding="utf-8"))
        self.buffers = [open(os.path.join(folder, b["uri"]), "rb").read() for b in self.json["buffers"]]

    def accessor(self, index):
        a = self.json["accessors"][index]
        view = self.json["bufferViews"][a["bufferView"]]
        dtype = np.dtype(DTYPES[a["componentType"]])
        n = COMPONENTS[a["type"]]
        start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = view.get("byteStride", dtype.itemsize * n)
        raw = self.buffers[view["buffer"]]
        out = np.empty((a["count"], n), dtype)
        if stride == dtype.itemsize * n:
            out[:] = np.frombuffer(raw, dtype, a["count"] * n, start).reshape(-1, n)
        else:
            for i in range(a["count"]):
                out[i] = np.frombuffer(raw, dtype, n, start + i * stride)
        if a.get("normalized"):
            out = out.astype(np.float32) / np.iinfo(dtype).max
        return out

    def local_matrix(self, node, drop_translation=False):
        n = self.json["nodes"][node]
        if "matrix" in n:
            m = np.array(n["matrix"], np.float64).reshape(4, 4).T  # column major
        else:
            t = np.array(n.get("translation", [0, 0, 0]), np.float64)
            x, y, z, w = n.get("rotation", [0, 0, 0, 1])
            r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                          [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                          [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
            s = np.array(n.get("scale", [1, 1, 1]), np.float64)
            m = np.eye(4)
            m[:3, :3] = r * s
            m[:3, 3] = t
        if drop_translation:
            m = m.copy()
            m[:3, 3] = 0.0
        return m

    def parents(self):
        p = {}
        for i, n in enumerate(self.json["nodes"]):
            for c in n.get("children", []):
                p[c] = i
        return p

    def world_matrix(self, node, parents, layout_node=None):
        # the chain up to the root; the LOD group's own offset (layout_node) is only a layout translation, dropped
        m = np.eye(4)
        while node is not None:
            m = self.local_matrix(node, drop_translation=node == layout_node) @ m
            node = parents.get(node)
        return m

    def image(self, material, slot):
        tex = material.get(slot) if slot != "baseColorTexture" else material.get("pbrMetallicRoughness", {}).get(slot)
        if not tex:
            return None
        source = self.json["textures"][tex["index"]]["source"]
        return os.path.join(self.folder, self.json["images"][source]["uri"])


def lod_of(name, group_name):
    """LOD index of a group's child, None for what isn't a tree LOD (billboards, references)"""
    low = name.lower()
    if "billboard" in low:
        return None
    for k in range(LODS):
        if low.endswith(f"_lod{k}") or low.endswith(f" lod{k}"):
            return k
    if "lod" not in low:
        return 0  # birch: the LOD0 child has no suffix
    return None


def find_trees(g):
    """[(group name, {lod: lod node})] for every tree group: a node with a LOD1 child"""
    trees = []
    for i, n in enumerate(g.json["nodes"]):
        name = n.get("name", "")
        if any(s in name.lower() for s in SKIP_GROUPS):
            continue
        children = n.get("children", [])
        lods = {}
        for c in children:
            k = lod_of(g.json["nodes"][c].get("name", ""), name)
            if k is not None and k not in lods:
                lods[k] = c
        if 1 in lods and 0 in lods:
            trees.append((name, lods))
    return trees


def collect(g, lod_node, parents, materials):
    """the bark + leaf triangles under a LOD node in the scene's space (metres, y up): positions, normals, uvs,
    colours (or None), leaf flag per vertex, indices"""
    P, N, U, C, L, I = [], [], [], [], [], []
    base = 0
    stack = [lod_node]
    while stack:
        node = stack.pop()
        n = g.json["nodes"][node]
        stack.extend(n.get("children", []))
        if "mesh" not in n:
            continue
        m = g.world_matrix(node, parents, layout_node=lod_node)
        nm = np.linalg.inv(m[:3, :3]).T
        for prim in g.json["meshes"][n["mesh"]]["primitives"]:
            kind = materials.get(prim.get("material"))
            if kind is None:
                continue
            attrs = prim["attributes"]
            pos = g.accessor(attrs["POSITION"]).astype(np.float64)
            nor = g.accessor(attrs["NORMAL"]).astype(np.float64)
            uv = g.accessor(attrs["TEXCOORD_0"]).astype(np.float32)
            col = g.accessor(attrs["COLOR_0"]).astype(np.float32)[:, :3] if "COLOR_0" in attrs else None
            idx = g.accessor(prim["indices"]).astype(np.uint32).ravel()
            pos = pos @ m[:3, :3].T + m[:3, 3]
            nor = nor @ nm.T
            nor /= np.maximum(np.linalg.norm(nor, axis=1, keepdims=True), 1e-9)
            P.append(pos); N.append(nor); U.append(uv); L.append(np.full(len(pos), kind == "leaf"))
            C.append(col if col is not None else np.full((len(pos), 3), np.nan, np.float32))
            I.append(idx + base)
            base += len(pos)
    if not P:
        return None
    return np.concatenate(P), np.concatenate(N), np.concatenate(U), np.concatenate(C), np.concatenate(L), np.concatenate(I)


def classify_materials(g):
    """material index -> 'bark' / 'leaf' / None (billboards, references), and the texture files of each kind"""
    kinds, files = {}, {}
    for i, mat in enumerate(g.json["materials"]):
        name = mat.get("name", "").lower()
        if any(s in name for s in ("billboard", "checker", "ref", "ground", "man", "filler")):
            kinds[i] = None
            continue
        kind = "leaf" if mat.get("alphaMode") in ("MASK", "BLEND") else "bark"
        kinds[i] = kind
        if kind not in files:  # the first one (the summer clusters in the oak pack: no season suffix)
            files[kind] = (g.image(mat, "baseColorTexture"), g.image(mat, "normalTexture"))
        elif kind == "leaf" and "_ex" not in name and "autumn" not in name and "winter" not in name and "spring" not in name:
            files[kind] = (g.image(mat, "baseColorTexture"), g.image(mat, "normalTexture"))
    return kinds, files


def bleed(rgba):
    """transparent texels take the colour of the nearest leaves (mips without dark fringes)"""
    a = rgba.astype(np.float32)
    rgb, solid = a[..., :3], a[..., 3] >= 128
    out, filled = rgb.copy(), solid.copy()
    for radius in (1, 2, 4, 8, 16, 32, 64, 128):
        w = np.asarray(Image.fromarray((filled * 255).astype(np.uint8)).filter(ImageFilter.BoxBlur(radius))).astype(np.float32) / 255.0
        acc = np.stack([np.asarray(Image.fromarray(np.clip(out[..., i] * filled, 0, 255).astype(np.uint8)).filter(ImageFilter.BoxBlur(radius))).astype(np.float32) for i in range(3)], -1)
        new = (~filled) & (w > 0.02)
        out[new] = (acc / np.maximum(w[..., None], 1e-4))[new]
        filled |= new
        if filled.all():
            break
    a[..., :3] = out
    return a.astype(np.uint8)


def save_texture(src, dst, alpha):
    img = Image.open(src).convert("RGBA" if alpha else "RGB")
    scale = min(1.0, MAX_TEXTURE / max(img.size))
    if scale < 1.0:
        img = img.resize((max(4, round(img.width * scale)), max(4, round(img.height * scale))), Image.LANCZOS)
    if alpha:
        img = Image.fromarray(bleed(np.asarray(img)), "RGBA")
    img.save(dst)
    return img


def tree_space(lods):
    """recentre on LOD0's trunk (the bark at the bottom), scale to the engine's size: returns the transform for
    every LOD and the tree's numbers (metres)"""
    pos, _, _, _, leaf, _ = lods[0]
    bark = pos[~leaf] if (~leaf).any() else pos
    bottom = bark[:, 1].min()
    height = pos[:, 1].max() - bottom
    low = bark[bark[:, 1] < bottom + 0.03 * height + 0.05]
    centre = np.array([low[:, 0].mean(), bottom, low[:, 2].mean()])
    radial = np.linalg.norm((pos - centre)[:, [0, 2]], axis=1)
    size = max(height / 1.08, radial.max() / 0.58)
    # the trunk's radius just above the root flare (collision): the innermost ring of the bark between 1 and 2 m up
    # (low branches, roots and second stems reach into that band too, they're further out)
    band = bark[(bark[:, 1] > bottom + 0.05 * height) & (bark[:, 1] < bottom + 0.12 * height)]
    if len(band) < 3:
        band = bark[bark[:, 1] < bottom + 0.15 * height]
    trunk = np.percentile(np.linalg.norm((band - centre)[:, [0, 2]], axis=1), 15) if len(band) else 0.02 * height
    return centre, size, height, trunk, radial.max()


def vertices(lod, centre, size, crown):
    """Trees::TreeVertex for one LOD: positionWind, normalMaterial (w = pack * 2 + leaf), uvAO (z = AO)"""
    pos, nor, uv, col, leaf, idx = lod
    p = (pos - centre) / size
    h = np.clip(p[:, 1] / max(p[:, 1].max(), 1e-6), 0.0, 1.0)
    r = np.linalg.norm(p[:, [0, 2]], axis=1) / max(crown, 1e-6)
    # sway: nothing at the base, most at the top and out along the branches
    wind = np.clip(h * 0.65 + np.clip(r, 0, 1) * 0.35, 0.0, 1.0) ** 1.2
    # ambient occlusion: darker inside the crown (towards its centre and the trunk), a little under it. the packs'
    # own baked AO (vertex colour R) where they have it
    lp = p[leaf] if leaf.any() else p
    c = lp.mean(axis=0)
    radii = np.maximum(np.percentile(np.abs(lp - c), 90, axis=0), 1e-3)
    q = np.linalg.norm((p - c) / radii, axis=1)
    ao = 0.35 + 0.65 * np.clip((q - 0.2) / 0.9, 0.0, 1.0)
    ao = np.where(leaf, ao * (0.85 + 0.15 * np.clip(nor[:, 1] * 0.5 + 0.5, 0, 1)), np.maximum(ao, 0.55) * (0.6 + 0.4 * h))
    baked = ~np.isnan(col[:, 0])
    ao[baked] = np.clip(col[baked, 0], 0.15, 1.0) * 0.8 + ao[baked] * 0.2
    # the leaf cards' normals: the packs' are the cards' own, pointing either way (only about half out of the crown),
    # so the crowns shaded as a noisy mix instead of a sunny side and a shaded one. every card's normal turned to
    # point out of the crown, then bent most of the way towards the crown's own (ellipsoid) normal there: the usual
    # "spherical" foliage normals, the crown shades as one soft volume and the cards keep a bit of their angle
    radial = (p - c) / (radii * radii)
    radial /= np.maximum(np.linalg.norm(radial, axis=1, keepdims=True), 1e-9)
    card = nor * np.where(np.einsum("ij,ij->i", nor, radial) < 0.0, -1.0, 1.0)[:, None]
    bent = card * 0.35 + radial * 0.65
    bent /= np.maximum(np.linalg.norm(bent, axis=1, keepdims=True), 1e-9)
    nor = np.where(leaf[:, None], bent, nor)
    return p, nor, uv, wind, ao, leaf, idx


def import_pack(g, pack_index, scale, chunks, meshes, kinds=None, rigid=False):
    """every tree (LOD group) of one pack into chunks (vertex chunks, index chunks, [vertex count]) and meshes.
    kinds: [(kind, group name regex)] for a shrub pack, only the groups one matches, its kind index in the name so the
    meshes sort by kind (write_tables). rigid: no sway"""
    materials, _ = classify_materials(g)
    vertex_chunks, index_chunks, vertex_count = chunks
    parents = g.parents()
    for name, lod_nodes in sorted(find_trees(g)):
        kind = None
        if kinds is not None:
            kind = next((k for k, (_, pattern) in enumerate(kinds) if re.search(pattern, name)), None)
            if kind is None:
                continue
        lods = [collect(g, lod_nodes[min(k, max(lod_nodes))], parents, materials) for k in range(LODS)]
        if any(l is None for l in lods) or (not lods[0][4].any() and kinds is None):
            print(f"  skipping {name}: no bark / leaves")
            continue
        lods = [(l[0] * scale,) + l[1:] for l in lods]
        centre, size, height, trunk, crown = tree_space(lods)
        table = []
        for lod in lods:
            p, nor, uv, wind, ao, leaf, idx = vertices(lod, centre, size, crown / size)
            if rigid:
                wind = np.zeros_like(wind)
            material = pack_index * 2 + leaf.astype(np.float32)
            v = np.zeros((len(p), 12), np.float32)
            v[:, 0:3], v[:, 3] = p, wind
            v[:, 4:7], v[:, 7] = nor, material
            v[:, 8:10], v[:, 10], v[:, 11] = uv, ao, 0.0
            table.append((sum(len(c) for c in index_chunks), len(idx)))
            index_chunks.append((idx + vertex_count[0]).astype(np.uint32))
            vertex_chunks.append(v)
            vertex_count[0] += len(p)
        young = height < YOUNG_HEIGHT and kinds is None
        sort_name = name if kind is None else f"{kind}_{name}"
        meshes.append((pack_index, sort_name, size, height, trunk / size, young, table, kind))
        print(f"{name:32s} {height:5.1f} m tall, size {size:5.1f} m, trunk {trunk * 100:4.0f} cm, "
              f"tris {[n // 3 for _, n in table]}{'  (young)' if young else ''}")


def save_pack_textures(g, out_dir, folder):
    _, files = classify_materials(g)
    os.makedirs(os.path.join(out_dir, folder), exist_ok=True)
    save_texture(files["bark"][0], os.path.join(out_dir, folder, "bark.png"), False)
    save_texture(files["bark"][1], os.path.join(out_dir, folder, "bark_normal.png"), False)
    if "leaf" in files:
        save_texture(files["leaf"][0], os.path.join(out_dir, folder, "leaves.png"), True)
        save_texture(files["leaf"][1], os.path.join(out_dir, folder, "leaves_normal.png"), False)
    else:  # (the cacti: bark only, the shaders still bind a leaf pair per pack)
        Image.new("RGBA", (4, 4), (0, 0, 0, 0)).save(os.path.join(out_dir, folder, "leaves.png"))
        Image.new("RGB", (4, 4), (128, 128, 255)).save(os.path.join(out_dir, folder, "leaves_normal.png"))


def read_existing_trees(root, chunks, meshes):
    """the tree packs' meshes back from assets/trees/trees.bin and the generated tables (--shrubs), the shrub packs'
    left out (they're re-imported)"""
    header = open(os.path.join(root, "src", "treemeshes.h"), encoding="utf-8").read()
    glsl = open(os.path.join(root, "src", "shaders", "treemeshes.glsl"), encoding="utf-8").read()

    def array(text, name, kind=int):
        body = re.search(name + r"\[[^\]]*\]\s*=\s*(?:\w+\[\]\()?\{?\s*([^;})]*)", text).group(1)
        return [kind(v.strip().rstrip("u")) for v in body.split(",") if v.strip()]
    order = array(header, "TREE_MESH_FILE_INDEX")
    folders = re.findall(r'"(\w+)"', re.search(r"TREE_PACK_FOLDERS[^;]*;", header).group(0))
    first, count, young = array(glsl, "PACK_FIRST"), array(glsl, "PACK_COUNT"), array(glsl, "PACK_YOUNG")
    sizes, trunks = array(glsl, "MESH_SIZE", float), array(glsl, "MESH_TRUNK", float)
    data = open(os.path.join(root, "assets", "trees", "trees.bin"), "rb").read()
    magic, n, lods, nv, ni = struct.unpack_from("<4sIIII", data, 0)
    assert magic == b"TRE1" and lods == LODS and n == len(order), "trees.bin doesn't match treemeshes.h"
    off = 20
    table = np.frombuffer(data, "<u4", n * lods * 2, off).reshape(n, lods, 2)
    off += n * lods * 8
    V = np.frombuffer(data, "<f4", nv * 12, off).reshape(nv, 12)
    off += nv * 48
    I = np.frombuffer(data, "<u4", ni, off)
    vertex_chunks, index_chunks, vertex_count = chunks
    tree_folders = [p[0] for p in PACKS]
    # in file order, so the trees' vertices and indices come out as they were
    engine_of = {f: e for e, f in enumerate(order)}
    for f in range(n):
        e = engine_of[f]
        pack = next(p for p in range(len(folders)) if first[p] <= e < first[p] + count[p])
        if folders[pack] not in tree_folders:
            continue
        assert tree_folders.index(folders[pack]) == pack, "the tree packs moved: re-import them from their downloads"
        lists = [I[a:a + c] for a, c in table[f]]
        used = np.unique(np.concatenate(lists))
        remap = np.zeros(used.max() + 1, np.uint32)
        remap[used] = np.arange(len(used), dtype=np.uint32) + vertex_count[0]
        out_table = []
        for l in lists:
            out_table.append((sum(len(c) for c in index_chunks), len(l)))
            index_chunks.append(remap[l])
        vertex_chunks.append(V[used].copy())
        vertex_count[0] += len(used)
        size = sizes[e] * METERS_PER_UNIT
        meshes.append((pack, f"{e:04d}", size, size * 1.08, trunks[e], e - first[pack] < young[pack], out_table, None))
    print(f"kept {len(meshes)} tree meshes from trees.bin")


def read_tree_credits(out_dir):
    """the tree packs' credit lines from the README this script wrote (--shrubs: no downloads to read them from)"""
    text = open(os.path.join(out_dir, "README.md"), encoding="utf-8").read()
    credits = []
    for folder, group, display, scale in PACKS:
        m = re.search(r"- \*\*" + display + r"\*\*: (.*)", text)
        credits.append((display, m.group(1).strip() if m else ""))
    return credits


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    shrubs_only = sys.argv[1] == "--shrubs"
    root = os.getcwd()
    out_dir = os.path.join(root, "assets", "trees")
    os.makedirs(out_dir, exist_ok=True)

    chunks = ([], [], [0])  # vertex chunks, index chunks, [vertex count]
    meshes = []  # (pack, name, size m, height m, trunk / size, young, [(firstIndex, indexCount) per LOD], shrub kind)
    credits = []
    if shrubs_only:
        read_existing_trees(root, chunks, meshes)
        credits = read_tree_credits(out_dir)
    else:
        source = sys.argv[1]
        for pack_index, (folder, group, display, scale) in enumerate(PACKS):
            g = Gltf(os.path.join(source, folder))
            save_pack_textures(g, out_dir, folder)
            lic = open(os.path.join(source, folder, "license.txt"), encoding="utf-8", errors="replace").read()
            credits.append((display, lic))
            print(f"== {display}")
            import_pack(g, pack_index, scale, chunks, meshes)
    first_new = len(meshes) if shrubs_only else 0
    incoming = os.path.join(root, "assets", "incoming")
    for s, (download, folder, display, scale, kinds, rigid) in enumerate(SHRUB_PACKS):
        g = Gltf(os.path.join(incoming, download))
        save_pack_textures(g, out_dir, folder)
        lic = open(os.path.join(incoming, download, "license.txt"), encoding="utf-8", errors="replace").read()
        credits.append((display, lic))
        print(f"== {display}")
        import_pack(g, len(PACKS) + s, scale, chunks, meshes, kinds, rigid)

    vertex_chunks, index_chunks, _ = chunks
    vertices_all = np.concatenate(vertex_chunks).astype(np.float32)
    indices_all = np.concatenate(index_chunks).astype(np.uint32)
    # some packs' LOD nodes are turned / offset against their LOD0 (young maples up to ~150 degrees): line them up
    # (align_tree_lods.py, which can also fix an existing trees.bin). the trees read back from trees.bin already are
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from align_tree_lods import align
    lod_table = np.array([[[first, count] for first, count in m[6]] for m in meshes[first_new:]], dtype=np.uint32)
    align(vertices_all, indices_all, lod_table)  # (float32, 12 per vertex: Trees::TreeVertex)
    # (written next to it and swapped in: a half written trees.bin fails the loader's checks)
    with open(os.path.join(out_dir, "trees.bin.tmp"), "wb") as f:
        f.write(struct.pack("<4sIIII", b"TRE1", len(meshes), LODS, len(vertices_all), len(indices_all)))
        for m in meshes:
            for first, count in m[6]:
                f.write(struct.pack("<II", first, count))
        f.write(vertices_all.tobytes())
        f.write(indices_all.tobytes())
    os.replace(os.path.join(out_dir, "trees.bin.tmp"), os.path.join(out_dir, "trees.bin"))
    print(f"{len(meshes)} meshes, {len(vertices_all)} vertices, {len(indices_all) // 3} triangles, "
          f"{os.path.getsize(os.path.join(out_dir, 'trees.bin')) / 1e6:.1f} MB")

    write_tables(root, meshes)
    write_readme(out_dir, credits)


def write_tables(root, meshes):
    # the tree packs, then the shrub packs: (folder, group, display, scale)
    all_packs = PACKS + [(folder, PACK_GROUP_SHRUB, display, scale) for _, folder, display, scale, _, _ in SHRUB_PACKS]
    packs = len(all_packs)
    count = [sum(1 for m in meshes if m[0] == p) for p in range(packs)]
    # per pack: the young ones first, then the grown ones (they're sorted by name, the saplings / small ones are
    # mixed in): placement picks from either range
    order = []
    for p in range(packs):
        ms = [i for i, m in enumerate(meshes) if m[0] == p]
        order += sorted(ms, key=lambda i: (not meshes[i][5], meshes[i][1]))
    young = [sum(1 for i in order if meshes[i][0] == p and meshes[i][5]) for p in range(packs)]
    # the meshes in that order (the tables index meshes in it, trees.bin is reordered by the loader with REMAP)
    lines_h = [
        "// generated by tools/import_trees.py, don't edit",
        "#pragma once",
        "#include <cstdint>",
        f"constexpr uint32_t TREE_PACKS = {packs};",
        f"constexpr uint32_t TREE_MESH_COUNT = {len(meshes)};",
        f"constexpr uint32_t TREE_LODS = {LODS};",
        "// the order the engine uses (young ones first per pack) -> the mesh in trees.bin",
        f"constexpr uint32_t TREE_MESH_FILE_INDEX[TREE_MESH_COUNT] = {{ {', '.join(str(i) for i in order)} }};",
        f"constexpr const char* TREE_PACK_FOLDERS[TREE_PACKS] = {{ {', '.join(chr(34) + p[0] + chr(34) for p in all_packs)} }};",
    ]
    open(os.path.join(root, "src", "treemeshes.h"), "w", encoding="utf-8", newline="\r\n").write("\n".join(lines_h) + "\n")

    def floats(values):
        return ", ".join(f"{v:.4f}" for v in values)
    sizes = [meshes[i][2] / METERS_PER_UNIT for i in order]
    trunks = [meshes[i][4] for i in order]
    lines = [
        "// generated by tools/import_trees.py, don't edit",
        "#ifndef TREE_MESHES_GLSL",
        "#define TREE_MESHES_GLSL",
        "",
        "// the imported tree packs (assets/trees): every mesh's natural size in world units (TreeInstance posScale.w,",
        "// what fits into the impostor card) and its trunk's radius relative to that. per pack its meshes are",
        "// consecutive, the young ones first",
        f"#define TREE_PACKS {packs}",
        f"#define TREE_MESHES {len(meshes)}",
        "#define PACK_GROUP_CONIFER 0",
        "#define PACK_GROUP_BROADLEAF 1",
        f"#define PACK_GROUP_SHRUB {PACK_GROUP_SHRUB} // the shrubs and small trees that were ground cover (trees_place.comp)",
    ]
    for p, (folder, group, display, scale) in enumerate(all_packs):
        lines.append(f"#define PACK_{folder.upper()} {p}")
    lines += [
        f"const uint PACK_GROUP[TREE_PACKS] = uint[]({', '.join(str(p[1]) + 'u' for p in all_packs)});",
        f"const uint PACK_FIRST[TREE_PACKS] = uint[]({', '.join(str(sum(count[:p])) + 'u' for p in range(packs))});",
        f"const uint PACK_COUNT[TREE_PACKS] = uint[]({', '.join(str(c) + 'u' for c in count)});",
        f"const uint PACK_YOUNG[TREE_PACKS] = uint[]({', '.join(str(y) + 'u' for y in young)}); // the first this many of each pack",
        f"const float MESH_SIZE[TREE_MESHES] = float[]({floats(sizes)});",
        f"const float MESH_TRUNK[TREE_MESHES] = float[]({floats(trunks)});",
        "",
        "// the shrub packs' kinds (SHRUB_PACKS in the script), each a consecutive range of meshes",
    ]
    kinds = [(f"{display}", kind) for _, _, display, _, ks, _ in SHRUB_PACKS for kind, _ in ks]
    kind_first, kind_count = [], []
    s = 0
    for p, (_, _, _, _, ks, _) in enumerate(SHRUB_PACKS):
        pack = len(PACKS) + p
        for k in range(len(ks)):
            ms = [e for e, i in enumerate(order) if meshes[i][0] == pack and meshes[i][7] == k]
            assert ms and ms == list(range(ms[0], ms[0] + len(ms))), "a shrub kind's meshes aren't consecutive"
            kind_first.append(ms[0])
            kind_count.append(len(ms))
    lines.append(f"#define SHRUB_KINDS {len(kinds)}")
    lines.append(f"#define FIRST_SHRUB_MESH {sum(count[:len(PACKS)])}u // the meshes from here on are shrubs")
    for k, (display, kind) in enumerate(kinds):
        lines.append(f"#define SHRUB_{kind} {k}")
    lines += [
        f"const uint SHRUB_FIRST[SHRUB_KINDS] = uint[]({', '.join(str(f) + 'u' for f in kind_first)});",
        f"const uint SHRUB_COUNT[SHRUB_KINDS] = uint[]({', '.join(str(c) + 'u' for c in kind_count)});",
        "",
        "#endif",
    ]
    open(os.path.join(root, "src", "shaders", "treemeshes.glsl"), "w", encoding="utf-8", newline="\r\n").write("\n".join(lines) + "\n")


def write_readme(out_dir, credits):
    text = ["# Trees", "",
            "Imported with `tools/import_trees.py` from the glTF downloads of these packs (the downloads themselves aren't",
            "in the repository: the script writes `trees.bin`, the textures here and `src/treemeshes.h` /",
            "`src/shaders/treemeshes.glsl`). All by LOLIPOP (https://sketchfab.com/lolipop_1707), CC-BY-4.0.", "",
            "The lilac, bush and cactus packs were ground cover first: their downloads are in `assets/incoming`",
            "(`tools/import_trees.py --shrubs` re-imports only them).", ""]
    for display, lic in credits:
        line = next((l for l in lic.splitlines() if l.startswith("This work is based on")), None)
        text.append(f"- **{display}**: {line or lic.strip()}")
    text.append("")
    open(os.path.join(out_dir, "README.md"), "w", encoding="utf-8", newline="\r\n").write("\n".join(text))


if __name__ == "__main__":
    main()
