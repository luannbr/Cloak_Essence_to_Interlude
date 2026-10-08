#!/usr/bin/env python3
"""Decoder for the rigid collar meshes of the Essence standard cloaks (<Body>_Hrm_ad11, <Body>_mNNN_Hrm_ad11, LineageAccessory3.<body>_pvp_Hrm_ad11 ...).

These are old multi-LOD skeletal meshes (2 bones: mantle_pin / mantle_up, every point 100 % on mantle_up). The object holds several LODs;
LOD 0 is the one that matches all of: faces (cidx F @ header+36, header = after the materials), 12-byte points and the 10-byte wedges.

  load(pkg_path, mesh_name) -> dict(points [(x,y,z)] , wedges [(point, u, v)], faces [(w0,w1,w2)], nverts)
"""
import os, sys, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2pkg
from l2pkg import Reader


def _cidx(d, p):
    r = Reader(d); r.p = p
    try:
        return r.cidx(), r.p
    except Exception:
        return None, p


def decode(pk, e):
    d = pk.read_obj(e)
    NP = struct.unpack_from('<i', d, 46)[0]
    r = Reader(d); r.p = 51; nm = r.cidx()
    for _ in range(nm): r.cidx()
    r.p += 36
    F = r.cidx()
    if not 8 <= F <= 4000 or not 8 <= NP <= 4000:
        raise ValueError('implausible header NP=%d F=%d' % (NP, F))
    # 1) face arrays: F triples of u16 (all distinct), the largest wedge index defines the wedge count
    cands = []
    for s in range(60, len(d) - 6 * F):
        ok = True; mx = 0
        for i in range(F):
            a, b, c = struct.unpack_from('<3H', d, s + 6 * i)
            if a == b or b == c or a == c or max(a, b, c) > 3 * NP:
                ok = False; break
            mx = max(mx, a, b, c)
        if ok:
            cands.append((s, mx + 1))
    if not cands:
        raise ValueError('faces not found in ' + e['name'])
    # 2) wedge arrays {u16 point, f32 u, f32 v}
    wedges = {}
    for p in range(60, len(d) - 100):
        n, q = _cidx(d, p)
        if n is None or not NP <= n <= 4 * NP or q + 10 * n > len(d):
            continue
        ok = True
        for i in range(n):
            pt, u, v = struct.unpack_from('<Hff', d, q + 10 * i)
            if pt >= NP or not (-2 < u < 4 and -2 < v < 4):
                ok = False; break
        if ok:
            wedges[n] = q
    # 3) point arrays (12 bytes)
    parrs = []
    for p in range(60, len(d) - 12 * NP):
        n, q = _cidx(d, p)
        if n != NP:
            continue
        P = [struct.unpack_from('<3f', d, q + 12 * i) for i in range(NP)]
        if all(all(math.isfinite(c) and abs(c) < 300 for c in v) for v in P):
            parrs.append(P)
    best = None
    # LOD 0 is the FIRST of everything: the earliest valid face array, the first wedge array of the same size after it, the first point array after that.
    # (The file also holds lower LODs whose arrays look valid too; picking by "smallest edges" mixed LODs and produced shredded collars.)
    wed_all = {}
    for p in range(60, len(d) - 100):
        n, q = _cidx(d, p)
        if n is None or not NP <= n <= 4 * NP or q + 10 * n > len(d):
            continue
        ok = True
        for i in range(n):
            pt, u, v = struct.unpack_from('<Hff', d, q + 10 * i)
            if pt >= NP or not (-2 < u < 4 and -2 < v < 4):
                ok = False; break
        if ok:
            wed_all.setdefault(n, []).append(q)
    pt_all = []
    for p in range(60, len(d) - 12 * NP):
        n, q = _cidx(d, p)
        if n != NP:
            continue
        P = [struct.unpack_from('<3f', d, q + 12 * i) for i in range(NP)]
        if all(all(math.isfinite(c) and abs(c) < 300 for c in v) for v in P):
            pt_all.append((q, P))
    for s, nw in sorted(cands):
        wq = [q for q in wed_all.get(nw, []) if q > s]
        if not wq:
            continue
        wq = min(wq)
        pp = [(q, P) for q, P in pt_all if q > wq]
        if not pp:
            continue
        q, P = min(pp, key=lambda x: x[0])
        W = [struct.unpack_from('<Hff', d, wq + 10 * i) for i in range(nw)]
        F_ = [struct.unpack_from('<3H', d, s + 6 * i) for i in range(F)]
        el = sorted(math.dist(P[W[a][0]], P[W[b][0]]) for a, b, c in F_)
        best = (el[len(el) // 2], P, W, F_)
        break
    if best is None:
        raise ValueError('no consistent (faces, wedges, points) in ' + e['name'])
    med, P, W, F_ = best
    return dict(name=e['name'], points=P, wedges=W, faces=F_, nverts=len(W), edge=med)


def load(path, mesh_name, pk=None):
    pk = pk or l2pkg.Package(path)
    low = mesh_name.lower()
    e = next((x for x in pk.exports if pk.cls_name(x['cls']) == 'SkeletalMesh' and x['name'].lower() == low), None)
    if e is None:
        raise KeyError(mesh_name)
    return decode(pk, e)


if __name__ == '__main__':
    root = os.environ.get('L2_ESSENCE_ROOT', '')
    m = load(root + r'\Animations\DarkElf.ukx', 'MDarkElf_Hrm_ad11')
    zs = [p[2] for p in m['points']]
    print(m['name'], len(m['points']), 'pts', len(m['wedges']), 'wedges', len(m['faces']), 'faces; median edge %.2f' % m['edge'], 'z %.1f..%.1f' % (min(zs), max(zs)))


def decode_rigid_cloak(pk, e):
    """a legacy-format cloak mesh that is rigid on its root bone (e.g. the dark elf / dwarf / orc Ranker cloaks: Cape_dummy + Cape_0) as the dict build_capes expects
    (points, wedges, faces, infl, bones, materials, sections); every point is 100 % on bone 0"""
    import essence
    m = decode(pk, e)
    d = pk.read_obj(e)
    sk = essence.parse_refskeleton(pk, d)
    if not sk:
        raise ValueError('skeleton not found in ' + e['name'])
    bones = sk[0][:]                                                 # (name, parent, quat, pos)
    r = Reader(d); r.p = 51; n = r.cidx(); refs = [r.cidx() for _ in range(n)]
    infl = [(1.0, i, 0) for i in range(len(m['points']))]
    return dict(name=e['name'], points=m['points'], wedges=m['wedges'], faces=m['faces'], infl=infl, bones=bones, materials=refs,
                sections=[(0, len(m['faces']))], npoints=len(m['points']))
