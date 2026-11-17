"""Converts Sketchfab glTF packs (assets/incoming/<pack>/scene.gltf, the unzipped downloads) into the layout the ground
cover loader (GroundCover::init) reads: assets/plants/<species>/<species>_1k.gltf + .bin + textures/.

    python tools/import_packs.py

Per species (SPECIES below): which packs, which of their plants (by name), which variants. For every plant:
  - the scene's transforms baked into the vertices (Sketchfab's root turns Z up into Y up and scales cm to m), the
    plant stood at the origin (its LOD0's footprint centred, its lowest point at 0), metres, Y up
  - its LODs kept: nodes <plant>__LOD0 / __LOD1 / __LOD2 (packs without LODs: __LOD0 only, the engine makes the rest)
  - billboards and the packs' reference objects (checker planes, the man for scale, reference images) left out
  - one primitive per material; textures as <stem>_diff_alpha_1k.png (an alpha that cuts something) or _diff_1k.jpg,
    and <stem>_nor_gl_1k.jpg, at most 1024 px (glTF normal maps are already OpenGL's +Y)
The licences (CC-BY) go to assets/plants/README.md by hand."""
import json, os, re, struct, shutil
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
INCOMING = os.path.join(ROOT, 'assets', 'incoming')
PLANTS = os.path.join(ROOT, 'assets', 'plants')
SKIP = re.compile(r'billboard|checker|man.?ref|ref.?man|ref_image|filler|ground', re.I)

# species folder: [(pack folder, plant name regex)]
SPECIES = {
    'lolipop_bush_big':     [('bush_models_pack_gameready_lods', r'^Bush_big_\d+$')],
    'lolipop_bush_small':   [('bush_models_pack_gameready_lods', r'^Bush_(small|medium)_\d+$')],
    'lilac_bush':           [('lilac_bush_pack_12_vars_lods_game_ready', r'^Lilac_(bush|small_bush)_\d+$')],
    'lilac_sapling':        [('lilac_bush_pack_12_vars_lods_game_ready', r'^Lilac_sapling_\d+$')],
    'lilac_tree':           [('lilac_bush_pack_12_vars_lods_game_ready', r'^Lilac_single_tree_\d+$')],
    'cactus':               [('cactus_pack_18_var_game_ready_lod_realistic', r'^Single_cactus_')],
    'cactus_clump':         [('cactus_pack_18_var_game_ready_lod_realistic', r'^(Small|Medium)_pack$')],
    'daisy':                [('daisy_models_pack_realistic_optimized', r'^Daisy_')],
    'grass_tufts':          [('grass_pack_lowpoly_game-ready', r'^Grass'), ('grass_pack_of_9_vars_lowpoly_game_ready', r'^Grass')],
    'undergrowth':          [('undergrowth_pack_lowpoly_game-ready', r'^(Dandelion|Lactuc|Plantain)')],
    'mushrooms':            [('lowpoly_mushrooms', r'^mushroom_\d+')],
}
# species whose scans come in arbitrary units: every plant scaled so its largest extent is this (metres)
NORMALISE = {'mushrooms': 0.12}


class Pack:
    def __init__(self, folder):
        self.folder = os.path.join(INCOMING, folder)
        self.g = json.load(open(os.path.join(self.folder, 'scene.gltf'), encoding='utf-8'))
        self.buffers = [open(os.path.join(self.folder, b['uri']), 'rb').read() for b in self.g['buffers']]
        self.parent = {}
        for i, n in enumerate(self.g['nodes']):
            for c in n.get('children', []): self.parent[c] = i

    def accessor(self, index):
        a = self.g['accessors'][index]
        view = self.g['bufferViews'][a['bufferView']]
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        dt = np.dtype({5126: '<f4', 5125: '<u4', 5123: '<u2', 5121: 'u1'}[a['componentType']])
        stride = view.get('byteStride', dt.itemsize * n)
        start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        raw = self.buffers[view['buffer']]
        if stride == dt.itemsize * n:
            return np.frombuffer(raw, dt, a['count'] * n, start).reshape(a['count'], n).astype(np.float64 if dt.kind == 'f' else np.int64)
        return np.array([np.frombuffer(raw, dt, n, start + i * stride) for i in range(a['count'])])

    def local(self, i):
        n = self.g['nodes'][i]
        if 'matrix' in n: return np.array(n['matrix'], np.float64).reshape(4, 4).T
        t = np.eye(4); t[:3, 3] = n.get('translation', [0, 0, 0])
        x, y, z, w = n.get('rotation', [0, 0, 0, 1])
        r = np.eye(4)
        r[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        s = np.diag(list(n.get('scale', [1, 1, 1])) + [1])
        return t @ r @ s

    def world(self, i):
        m = self.local(i)
        while i in self.parent:
            i = self.parent[i]
            m = self.local(i) @ m
        return m

    def mesh_nodes(self, i):
        """the mesh nodes at and under node i"""
        out, stack = [], [i]
        while stack:
            k = stack.pop()
            if SKIP.search(self.g['nodes'][k].get('name', '')): continue
            if 'mesh' in self.g['nodes'][k]: out.append(k)
            stack.extend(self.g['nodes'][k].get('children', []))
        return out

    def plants(self, pattern):
        """[(plant name, [mesh nodes per LOD])]: LOD groups (<name>_LOD0..2) where the pack has them, else every mesh
        node of its own whose name matches"""
        names = {i: n.get('name', '') for i, n in enumerate(self.g['nodes'])}
        found = []
        lod0 = {i: nm[:-5] for i, nm in names.items() if nm.endswith('_LOD0') and 'mesh' not in self.g['nodes'][i]}
        if lod0:
            for i, base in lod0.items():
                if not re.search(pattern, base): continue
                lods = [self.mesh_nodes(i)]
                for k in (1, 2):
                    j = next((jj for jj, nm in names.items() if nm == f'{base}_LOD{k}'), None)
                    if j is not None: lods.append(self.mesh_nodes(j))
                found.append((base, lods))
        else:
            for i, nm in names.items():
                if 'mesh' in self.g['nodes'][i] and re.search(pattern, nm) and not SKIP.search(nm):
                    found.append((re.sub(r'[^A-Za-z0-9]+', '_', nm).strip('_'), [[i]]))
        return sorted(found)


def convert(species, sources):
    out_dir = os.path.join(PLANTS, species)
    if os.path.exists(out_dir): shutil.rmtree(out_dir)
    os.makedirs(os.path.join(out_dir, 'textures'))
    blob = bytearray()
    views, accessors, meshes, nodes, materials, images, textures = [], [], [], [], [], [], []
    material_of = {}  # (pack, material index) -> output material

    def add(arr, kind, comp, target, minmax=False):
        while len(blob) % 4: blob.append(0)
        off = len(blob)
        data = np.ascontiguousarray(arr, '<f4' if comp == 5126 else '<u4')
        blob.extend(data.tobytes())
        views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(blob) - off, 'target': target})
        a = {'bufferView': len(views) - 1, 'componentType': comp, 'count': len(arr), 'type': kind}
        if minmax: a['min'], a['max'] = arr.min(0).tolist(), arr.max(0).tolist()
        accessors.append(a)
        return len(accessors) - 1

    def material(pack, tag, index):
        key = (tag, index)
        if key in material_of: return material_of[key]
        m = pack.g['materials'][index]
        stem = re.sub(r'[^A-Za-z0-9]+', '_', f"{species}_{m.get('name', 'mat')}").strip('_').lower()
        def image(tex):
            src = pack.g['images'][pack.g['textures'][tex['index']]['source']]['uri']
            return Image.open(os.path.join(pack.folder, src))
        base = image(m['pbrMetallicRoughness']['baseColorTexture']).convert('RGBA')
        base.thumbnail((1024, 1024))
        alpha = np.asarray(base)[:, :, 3]
        if (alpha < 128).mean() > 0.01:
            base.save(os.path.join(out_dir, 'textures', f'{stem}_diff_alpha_1k.png'))
        base.convert('RGB').save(os.path.join(out_dir, 'textures', f'{stem}_diff_1k.jpg'), quality=92)
        if 'normalTexture' in m:
            n = image(m['normalTexture']).convert('RGB'); n.thumbnail((1024, 1024))
        else:
            n = Image.new('RGB', (64, 64), (128, 128, 255))
        n.save(os.path.join(out_dir, 'textures', f'{stem}_nor_gl_1k.jpg'), quality=95)
        images.append({'uri': f'textures/{stem}_diff_1k.jpg'})
        textures.append({'source': len(images) - 1})
        materials.append({'name': stem, 'pbrMetallicRoughness': {'baseColorTexture': {'index': len(textures) - 1}}, 'doubleSided': True})
        material_of[key] = len(materials) - 1
        return material_of[key]

    report = []
    for tag, pattern in sources:
        pack = Pack(tag)
        for name, lods in pack.plants(pattern):
            parts = []  # per LOD: [(positions, normals, uvs, indices, material)]
            for lod_nodes in lods:
                prims = []
                for k in lod_nodes:
                    w = pack.world(k)
                    nm = np.linalg.inv(w[:3, :3]).T
                    for p in pack.g['meshes'][pack.g['nodes'][k]['mesh']]['primitives']:
                        if 'indices' not in p or 'TEXCOORD_0' not in p['attributes'] or 'material' not in p: continue
                        pos = pack.accessor(p['attributes']['POSITION'])
                        pos = (np.c_[pos, np.ones(len(pos))] @ w.T)[:, :3]
                        nor = pack.accessor(p['attributes']['NORMAL']) @ nm.T if 'NORMAL' in p['attributes'] else np.tile([0, 1, 0], (len(pos), 1))
                        nor /= np.maximum(np.linalg.norm(nor, axis=1, keepdims=True), 1e-9)
                        uv = pack.accessor(p['attributes']['TEXCOORD_0'])
                        idx = pack.accessor(p['indices']).reshape(-1)
                        if np.linalg.det(w[:3, :3]) < 0: idx = idx.reshape(-1, 3)[:, ::-1].reshape(-1)
                        prims.append((pos, nor, uv, idx, material(pack, tag, p['material'])))
                if prims: parts.append(prims)
            if not parts: continue
            # stood at the origin: the LOD0's footprint centred, its lowest point at 0
            allpos = np.concatenate([q[0] for q in parts[0]])
            shift = np.array([(allpos[:, 0].min() + allpos[:, 0].max()) / 2, allpos[:, 1].min(), (allpos[:, 2].min() + allpos[:, 2].max()) / 2])
            size = allpos.max(0) - allpos.min(0)
            scale = NORMALISE[species] / size.max() if species in NORMALISE else 1.0
            size = size * scale
            for lod, prims in enumerate(parts):
                # (the packs lay their LODs out in a row, metres apart: each LOD centred on its own footprint, its own
                # lowest point at 0, the same scale)
                lp = np.concatenate([q[0] for q in prims])
                shift = np.array([(lp[:, 0].min() + lp[:, 0].max()) / 2, lp[:, 1].min(), (lp[:, 2].min() + lp[:, 2].max()) / 2])
                gl_prims = []
                for pos, nor, uv, idx, mat in prims:
                    gl_prims.append({'attributes': {'POSITION': add((pos - shift) * scale, 'VEC3', 5126, 34962, True), 'NORMAL': add(nor, 'VEC3', 5126, 34962),
                                                    'TEXCOORD_0': add(uv, 'VEC2', 5126, 34962)},
                                     'indices': add(idx.astype(np.uint32), 'SCALAR', 5125, 34963), 'material': mat})
                meshes.append({'name': f'{name}__LOD{lod}', 'primitives': gl_prims})
                nodes.append({'name': f'{name}__LOD{lod}', 'mesh': len(meshes) - 1})
            tris = [sum(len(q[3]) // 3 for q in prims) for prims in parts]
            report.append(f'   {name:34s} {size[0]:5.2f} x {size[1]:5.2f} x {size[2]:5.2f} m  tris {tris}')

    with open(os.path.join(out_dir, f'{species}.bin'), 'wb') as f: f.write(blob)
    gltf = {'asset': {'version': '2.0', 'generator': 'tools/import_packs.py'}, 'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}],
            'nodes': nodes, 'meshes': meshes, 'materials': materials, 'textures': textures, 'images': images,
            'buffers': [{'uri': f'{species}.bin', 'byteLength': len(blob)}], 'bufferViews': views, 'accessors': accessors}
    json.dump(gltf, open(os.path.join(out_dir, f'{species}_1k.gltf'), 'w'), indent=1)
    print(f'{species}: {len(report)} plants, {len(materials)} materials')
    print('\n'.join(report))


if __name__ == '__main__':
    for species, sources in SPECIES.items():
        convert(species, sources)
