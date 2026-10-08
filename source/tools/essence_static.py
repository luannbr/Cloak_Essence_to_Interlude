#!/usr/bin/env python3
"""Decoder for the effect StaticMeshes of the Essence client (.usx, package version 133).

Layout found by inspection (UE2 UStaticMesh, L2 flavour):
  tagged properties (Materials = array of struct{Material: object ref, ...}) | FBox (6 floats + valid byte) | sphere | sections |
  vertex stream  = cidx n + n x {pos xyz, normal xyz} (24 bytes)
  colour stream  = int tag + cidx n + n x BGRA          (twice: colour, alpha)
  UV streams     = int tag + cidx count + for each: cidx n + n x {u, v} ...
  index buffer   = cidx m + m x u16  (triangle list)

  decode(pk, export) -> dict(verts [(x,y,z)], normals, uvs [(u,v)], tris [(a,b,c)], materials [(package, group, name, class)], bbox)
"""
import os, sys, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2pkg
from l2pkg import Reader


def _unit(v):
    n = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return 0.97 < n < 1.03


def _vertex_run(d, start=0):
    """the longest run of 24-byte {pos, unit normal} records; returns (offset, count)"""
    best = (0, 0)
    p = start
    L = len(d)
    while p + 24 * 3 <= L:
        n = 0
        q = p
        while q + 24 <= L:
            vals = struct.unpack_from('<6f', d, q)
            if not all(math.isfinite(x) and abs(x) < 20000 for x in vals) or not _unit(vals[3:6]):
                break
            n += 1
            q += 24
        if n >= 3:
            # a record can only start where the previous cidx count says so: prefer the run whose preceding byte(s) hold the count
            if n > best[1]:
                best = (p, n)
            p = q
        else:
            p += 1
    return best


def _count_ok(d, p, n):
    """the cidx count byte(s) in front of an array of n records"""
    r = Reader(d)
    for back in (1, 2, 3):
        if p - back < 0:
            continue
        r.p = p - back
        try:
            if r.cidx() == n and r.p == p:
                return True
        except Exception:
            pass
    return False


def _materials(pk, d):
    """Materials property (first thing in the object): array of struct{... Material: object ...} -> [(import tuple | export name)]"""
    out = []
    r = Reader(d)
    r.p = 0
    try:
        ni = r.cidx()
        if pk.names[ni] != 'Materials':
            return out
        info = r.u8()
        sz = (info >> 4) & 7
        size = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(sz)
        if size is None:
            size = r.u8() if sz == 5 else (r.u16() if sz == 6 else r.u32())
        end = r.p + size
        cnt = r.cidx()
        for _ in range(cnt):
            ref = 0
            while True:
                nm = pk.names[r.cidx()]
                if nm == 'None':
                    break
                inf = r.u8()
                typ = inf & 15
                s = (inf >> 4) & 7
                sname = pk.names[r.cidx()] if typ == 10 else None
                sz2 = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(s)
                if sz2 is None:
                    sz2 = r.u8() if s == 5 else (r.u16() if s == 6 else r.u32())
                if typ == 5:
                    ref = Reader(d[r.p:r.p + sz2]).cidx()
                if typ != 3:
                    r.p += sz2
            out.append(ref)
    except Exception:
        pass
    return out


def resolve(pk, ref):
    """object ref -> (package, group, name, class)"""
    if ref == 0:
        return None
    if ref > 0:
        e = pk.exports[ref - 1]
        return (os.path.basename(pk.path), '', e['name'], pk.cls_name(e['cls']))
    imp = pk.imports[-ref - 1]                       # (class package, class name, outer, object name)
    parts = []
    outer = imp[2]
    while outer < 0:
        o = pk.imports[-outer - 1]
        parts.append(o[3])
        outer = o[2]
    parts.reverse()
    return (parts[0] if parts else '', '.'.join(parts[1:]), imp[3], imp[1])


def decode(pk, e):
    d = pk.read_obj(e)
    mats = [resolve(pk, r) for r in _materials(pk, d)]
    p, n = _vertex_run(d, 0)
    if n < 3:
        raise ValueError('no vertex stream in ' + e['name'])
    # trim records until the count in front matches (the run can swallow following float data)
    for cand in range(n, 2, -1):
        if _count_ok(d, p, cand):
            n = cand
            break
    verts = []
    normals = []
    for i in range(n):
        v = struct.unpack_from('<6f', d, p + 24 * i)
        verts.append(v[:3])
        normals.append(v[3:])
    q = p + 24 * n
    # UV array: cidx n + n x (u, v) after the colour streams; take the first plausible one
    uvs = None
    for s in range(q, len(d) - 8 * n - 1):
        if d[s] != n and not (n >= 64 and _count_ok(d, s + 1, n)):
            continue
        ok = True
        for i in range(n):
            u, v = struct.unpack_from('<2f', d, s + 1 + 8 * i) if d[s] == n else (0, 0)
            if not (math.isfinite(u) and math.isfinite(v) and -64 < u < 64 and -64 < v < 64):
                ok = False
                break
        if ok and d[s] == n:
            uvs = [struct.unpack_from('<2f', d, s + 1 + 8 * i) for i in range(n)]
            uv_end = s + 1 + 8 * n
            break
    if uvs is None:
        uvs = [(0.0, 0.0)] * n
        uv_end = q
    # index buffer: cidx m + m x u16, m % 3 == 0, all indices < n, covering nearly all vertices
    tris = None
    best = None
    for s in range(uv_end, len(d) - 7):
        r = Reader(d)
        r.p = s
        try:
            m = r.cidx()
        except Exception:
            continue
        if m < 3 or m % 3 or r.p + 2 * m > len(d):
            continue
        idx = struct.unpack_from('<%dH' % m, d, r.p)
        if max(idx) >= n:
            continue
        used = len(set(idx))
        if used < n * 0.8:
            continue
        if any(idx[i] == idx[i + 1] or idx[i + 1] == idx[i + 2] or idx[i] == idx[i + 2] for i in range(0, m, 3)):
            continue
        best = idx
        break
    if best is None:
        raise ValueError('no index buffer in ' + e['name'])
    tris = [tuple(best[i:i + 3]) for i in range(0, len(best), 3)]
    bb = struct.unpack_from('<6f', d, 0)
    return dict(name=e['name'], verts=verts, normals=normals, uvs=uvs, tris=tris, materials=mats)


def find(pk, name, group=None):
    for e in pk.exports:
        if e['name'].lower() == name.lower() and pk.cls_name(e['cls']) == 'StaticMesh':
            return e
    return None


if __name__ == '__main__':
    S = os.path.join(os.environ.get('L2_ESSENCE_ROOT', ''), 'StaticMeshes')
    pk = l2pkg.Package(os.path.join(S, sys.argv[1]))
    for nm in sys.argv[2:]:
        e = find(pk, nm)
        m = decode(pk, e)
        xs = [v[0] for v in m['verts']]
        ys = [v[1] for v in m['verts']]
        zs = [v[2] for v in m['verts']]
        print(nm, len(m['verts']), 'verts', len(m['tris']), 'tris', 'bbox x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]' % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)), m['materials'])
