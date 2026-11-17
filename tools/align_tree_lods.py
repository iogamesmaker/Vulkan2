"""Lines up every tree's coarser LODs with its LOD0 in assets/trees/trees.bin (written by tools/import_trees.py).

Some trees in the packs have LOD nodes that are turned or offset differently from their LOD0 (a few young maples are
turned up to ~80 degrees and off centre by a tenth of their size): with the LOD crossfade in trees.comp the tree visibly
turned into another one as you walked past the switch distance. Per tree and LOD this
  - moves the LOD so its trunk base (the lowest bark) sits on LOD0's
  - turns it about the trunk to where its crown overlaps LOD0's best (a coarse 3D occupancy grid of LOD0, coarse to
    fine search over the angle), only when that's clearly better than not turning it
positions and normals both; the sway weights, uvs and indices are untouched.

    python tools/align_tree_lods.py            (rewrites assets/trees/trees.bin in place)
    python tools/align_tree_lods.py --dry-run  (only reports)
import_trees.py calls align() on what it writes, so a re-import keeps the fix."""
import struct
import sys
import os
import numpy as np

GRID = 28


def _base(points, bark):
    """the trunk's foot: the lowest bark (xz centroid of the bark in the bottom 6% of the height)"""
    p = points[bark] if bark.any() else points
    low = p[p[:, 1] <= p[:, 1].min() + 0.06 * (points[:, 1].max() - points[:, 1].min())]
    return low[:, [0, 2]].mean(0)


def _occupancy(points, lo, hi):
    cells = np.clip(((points - lo) / (hi - lo) * GRID).astype(int), 0, GRID - 1)
    grid = np.zeros((GRID, GRID, GRID), bool)
    grid[cells[:, 0], cells[:, 1], cells[:, 2]] = True
    # dilate by one cell: a near miss isn't a miss
    d = grid.copy()
    for axis in range(3):
        for s in (-1, 1):
            d |= np.roll(grid, s, axis)
    return d


def _yaw(points, centre, angle):
    c, s = np.cos(angle), np.sin(angle)
    q = points.copy()
    x, z = q[:, 0] - centre[0], q[:, 2] - centre[1]
    q[:, 0] = centre[0] + c * x + s * z
    q[:, 2] = centre[1] - s * x + c * z
    return q


def align(V, I, table, report=print):
    """V: (n, 12) float32 vertices (positionWind, normalMaterial, uvAO), I: indices, table: (meshes, lods, 2) first index /
    count. modifies V in place, returns how many LODs were moved"""
    moved = 0
    rng = np.random.default_rng(1)
    for m in range(table.shape[0]):
        f0, c0 = table[m, 0]
        v0 = np.unique(I[f0:f0 + c0])
        p0 = V[v0, :3].astype(np.float64)
        bark0 = (np.round(V[v0, 7]).astype(int) % 2) == 0
        b0 = _base(p0, bark0)
        lo, hi = p0.min(0) - 1e-3, p0.max(0) + 1e-3
        occ = _occupancy(p0, lo, hi)
        for lod in range(1, table.shape[1]):
            f, c = table[m, lod]
            v = np.unique(I[f:f + c])
            p = V[v, :3].astype(np.float64)
            bark = (np.round(V[v, 7]).astype(int) % 2) == 0
            shift = b0 - _base(p, bark)
            p[:, 0] += shift[0]
            p[:, 2] += shift[1]
            sample = p[rng.choice(len(p), min(len(p), 3000), replace=False)]

            def cost(angle):
                q = _yaw(sample, b0, angle)
                cells = ((q - lo) / (hi - lo) * GRID).astype(int)
                inside = np.all((cells >= 0) & (cells < GRID), axis=1)
                hit = np.zeros(len(q), bool)
                hit[inside] = occ[cells[inside, 0], cells[inside, 1], cells[inside, 2]]
                return 1.0 - hit.mean()

            angles = np.radians(np.arange(0.0, 360.0, 5.0))
            costs = [cost(a) for a in angles]
            best = angles[int(np.argmin(costs))]
            for step in (np.radians(1.0), np.radians(0.25)):
                fine = best + np.arange(-5, 6) * step
                fc = [cost(a) for a in fine]
                best = fine[int(np.argmin(fc))]
            c_none, c_best = cost(0.0), cost(best)
            turn = c_best < c_none * 0.85 and c_none - c_best > 0.02
            angle = best if turn else 0.0
            if not turn:  # (only the clearly misplaced ones: the trunk base estimate wobbles by a few % on the rest)
                continue
            if turn:
                moved += 1
                report(f"mesh {m} lod {lod}: moved {np.linalg.norm(shift):.3f}, turned {np.degrees(angle):.1f} deg (miss {c_none:.2f} -> {cost(angle):.2f})")
            q = _yaw(p, b0, angle)
            V[v, :3] = q.astype(np.float32)
            n = V[v, 4:7].astype(np.float64)
            nr = _yaw(np.column_stack([n[:, 0], np.zeros(len(n)), n[:, 2]]), (0.0, 0.0), angle)
            V[v, 4] = nr[:, 0].astype(np.float32)
            V[v, 6] = nr[:, 2].astype(np.float32)
    return moved


def main():
    path = os.path.join(os.path.dirname(__file__), '..', 'assets', 'trees', 'trees.bin')
    data = bytearray(open(path, 'rb').read())
    magic, meshes, lods, nv, ni = struct.unpack_from('<4s4I', data, 0)
    assert magic == b'TRE1'
    off = 20
    table = np.frombuffer(data, dtype='<u4', count=meshes * lods * 2, offset=off).reshape(meshes, lods, 2)
    off += meshes * lods * 8
    vstart = off
    V = np.frombuffer(data, dtype='<f4', count=nv * 12, offset=off).reshape(nv, 12).copy()
    off += nv * 48
    I = np.frombuffer(data, dtype='<u4', count=ni, offset=off)
    moved = align(V, I, table)
    print(f"{moved} LODs moved")
    if '--dry-run' not in sys.argv:
        data[vstart:vstart + nv * 48] = V.astype('<f4').tobytes()
        open(path, 'wb').write(data)
        print("written", path)


if __name__ == '__main__':
    main()
