#!/usr/bin/env python3
"""Decoder for the effect VertMeshes of the Essence client (LineageEffectMeshes*.ukx): vertex-animated meshes.

Layout found by inspection of `wing_high` (113 KB):  properties (Materials) | bounding box | sphere | ints | float frames
  frames : NF x NV x 3 float32 positions (NV and NF are found from the frame-to-frame continuity)
  after  : a material reference, scale / zero fields, then  cidx T + T x { u16 ?, u16 v0, u16 v1, u16 v2 }  (triangle list)
UVs are not stored per vertex in the same place as in static meshes; they are projected from the first frame (see planar_uv).

  decode(pk, export) -> dict(nv, nf, frames (nf, nv, 3), tris [(a, b, c)])
"""
import os, sys, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import l2pkg
from l2pkg import Reader


def _float_run(d):
    """(start, count) of the longest run of plausible float32 values"""
    best = (0, 0)
    for ph in range(4):
        run = 0
        start = ph
        for off in range(ph, len(d) - 4, 4):
            v = struct.unpack_from('<f', d, off)[0]
            if math.isfinite(v) and abs(v) < 600 and (abs(v) > 1e-7 or v == 0.0):
                if run == 0:
                    start = off
                run += 1
            else:
                if run > best[1]:
                    best = (start, run)
                run = 0
        if run > best[1]:
            best = (start, run)
    return best


def decode(pk, e):
    d = pk.read_obj(e)
    start, n = _float_run(d)
    n3 = n // 3
    if n3 < 100:
        raise ValueError('no frame data in ' + e['name'])
    arr = np.frombuffer(d, '<f4', n3 * 3, start).reshape(-1, 3)
    # the frame run can start a few floats early (neighbouring header floats): try small shifts and every vertex count
    best = None
    for shift in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11):
        if best is not None and best[3] == 0 and best[0] < 0.1:
            break
        a = arr[shift:]
        m = (len(a) // 1)
        for V in range(8, 400):
            F = len(a) // V
            if F < 2:
                continue
            A = a[:F * V].reshape(F, V, 3)
            d1 = np.abs(A[1:] - A[:-1]).mean()
            dv = np.abs(A[:, 1:] - A[:, :-1]).mean()
            score = d1 / (dv + 1e-6)
            if len(a) % V == 0:
                score *= 0.5                                         # an exact multiple: the frame block is the whole run
            if best is None or score < best[0]:
                best = (score, V, F, shift)
    score, V, F, shift = best
    if score > 0.2:
        raise ValueError('frame layout not found in %s (best score %.2f)' % (e['name'], score))
    frames = arr[shift:shift + V * F].reshape(F, V, 3).copy()
    dif = np.abs(frames[1:] - frames[:-1]).mean(axis=(1, 2))              # trailing floats of the header / the next block are not frames: cut where the continuity breaks
    lim = max(float(np.median(dif)) * 6.0, 0.05)
    bad = np.nonzero(dif > lim)[0]
    if len(bad):
        F = int(bad[0]) + 1
        frames = frames[:F].copy()
    end = start + (shift + V * F) * 12
    tris = triangulate(frames)
    return dict(name=e['name'], nv=V, nf=F, frames=frames, tris=tris)


def triangulate(frames):
    """The stored triangle list is not usable (indices refer to a different table), but the vertices of a wing are a regular sheet: Delaunay in the
    (y, z) plane of a middle frame, one wing (sign of y) at a time, dropping the long skinny triangles on the boundary."""
    from scipy.spatial import Delaunay
    P = frames[len(frames) // 2]
    tris = []
    for side in (P[:, 1] >= 0, P[:, 1] < 0):
        idx = np.nonzero(side)[0]
        if len(idx) < 3:
            continue
        dl = Delaunay(P[idx][:, 1:3])
        for s in dl.simplices:
            a, b, c = (int(idx[i]) for i in s)
            e1, e2, e3 = (np.linalg.norm(P[a] - P[b]), np.linalg.norm(P[b] - P[c]), np.linalg.norm(P[c] - P[a]))
            if max(e1, e2, e3) < 16.0:
                tris.append((a, b, c))
    return tris


def planar_uv(frames, mode='yz'):
    """UVs from the first frame: u grows outwards from the middle (|y|), v runs from the top (high z) down; both wings share the same map"""
    F0 = frames[0]
    y = np.abs(F0[:, 1])
    z = F0[:, 2]
    ymax = max(float(y.max()), 1e-3)
    zmin, zmax = float(z.min()), float(z.max())
    u = 0.04 + 0.92 * y / ymax
    v = 1.0 - (z - zmin) / max(zmax - zmin, 1e-3)
    return np.stack([u, v], 1)


def find(pk, name):
    for e in pk.exports:
        if e['name'].lower() == name.lower() and pk.cls_name(e['cls']) == 'VertMesh':
            return e
    return None


if __name__ == '__main__':
    R = os.path.join(os.environ.get('L2_ESSENCE_ROOT', ''), 'Animations')
    pk = l2pkg.Package(os.path.join(R, sys.argv[1]))
    for nm in sys.argv[2:]:
        m = decode(pk, find(pk, nm))
        fr = m['frames']
        print(nm, 'verts', m['nv'], 'frames', m['nf'], 'tris', len(m['tris']), 'bbox', fr[0].min(0).round(1), fr[0].max(0).round(1))
