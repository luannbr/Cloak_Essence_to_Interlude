"""Skinning helpers for the Essence mantles (bind pose, animated pose) + convention search.
   python essence_skin.py <ukx> <MeshName>      -> prints which quaternion convention places the bones on their vertices"""
import sys, os, math
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence import Package, parse_mesh, parse_anim


def quat_mat(q, conj=False):
    x, y, z, w = q
    if conj:
        x, y, z = -x, -y, -z
    n = x * x + y * y + z * z + w * w
    s = 2.0 / n if n > 0 else 0.0
    xx, yy, zz = x * x * s, y * y * s, z * z * s
    xy, xz, yz = x * y * s, x * z * s, y * z * s
    wx, wy, wz = w * x * s, w * y * s, w * z * s
    return np.array([[1 - yy - zz, xy - wz, xz + wy],
                     [xy + wz, 1 - xx - zz, yz - wx],
                     [xz - wy, yz + wx, 1 - xx - yy]])


def global_transforms(parents, locals_, conj_root=False, conj_all=False, order='parent_first'):
    """locals_[i] = (R3x3, t3) relative to the parent. Returns list of (R, t) in mesh space (column-vector convention)."""
    G = [None] * len(parents)
    for i, (R, t) in enumerate(locals_):
        p = parents[i]
        if i == 0 or p < 0 or p == i:
            G[i] = (R, np.array(t, float))
        else:
            Rp, tp = G[p]
            G[i] = (Rp @ R, Rp @ np.array(t, float) + tp)
    return G


def bind_locals(bones, conj_root, conj_all, flip_pos=False):
    out = []
    for i, (name, par, q, pos) in enumerate(bones):
        c = conj_root if i == 0 else conj_all
        out.append((quat_mat(q, conj=c), np.array(pos, float)))
    return out


def evaluate_conventions(mesh):
    P = np.array(mesh['points'])
    infl = mesh['infl']
    bones = mesh['bones']
    parents = [b[1] for b in bones]
    nb = len(bones)
    cen = np.zeros((nb, 3)); wsum = np.zeros(nb)
    for w, pt, bi in infl:
        cen[bi] += w * P[pt]; wsum[bi] += w
    cen = cen / np.maximum(wsum, 1e-9)[:, None]
    res = []
    for conj_root in (False, True):
        for conj_all in (False, True):
            G = global_transforms(parents, bind_locals(bones, conj_root, conj_all))
            pos = np.array([g[1] for g in G])
            # distance of every bone origin to the weighted centroid of its vertices (only bones that own vertices)
            d = np.linalg.norm(pos - cen, axis=1)[wsum > 5]
            res.append((float(np.median(d)), conj_root, conj_all, pos))
    res.sort(key=lambda r: r[0])
    return res


if __name__ == '__main__':
    pk = Package(sys.argv[1])
    e = next(x for x in pk.exports if x['name'] == sys.argv[2])
    m = parse_mesh(pk, e)
    print('mesh', m['name'], 'points', len(m['points']), 'bones', len(m['bones']))
    for med, cr, ca, pos in evaluate_conventions(m):
        print('conj_root=%s conj_all=%s  median |bone - vertex centroid| = %.2f' % (cr, ca, med))
    best = evaluate_conventions(m)[0]
    print('best bone origins:')
    for (name, par, q, p), g in zip(m['bones'], best[3]):
        print('  %-14s parent %2d  global (%7.2f %7.2f %7.2f)' % (name, par, *g))
    P = np.array(m['points']); print('vertex bbox', P.min(0).round(1), P.max(0).round(1))
