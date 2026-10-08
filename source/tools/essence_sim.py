#!/usr/bin/env python3
"""Decoder for the Essence cloth meshes (SimulationMesh) of the standard cloaks: <Body>Simulation.uix.

  load(pkg_path, mesh_name) -> dict(
       verts   [(x, y, z, nx, ny, nz, u, v)] * NP   rest pose (body mesh space, z up, the cloak hangs at -y), one entry per sim particle
       tris    [(a, b, c)] * F                        indices into verts
       anchors [particle index]                       pinned to the body (rows 0..1 of the grid)
       width   grid width (particles per row; rows = NP / width)
       springs [(i, j, rest)]                          unique, from the 8 neighbour tables of every particle (4 structural + 4 diagonal)
       capsules[(boneA, boneB, radius, sphereA, sphereB)]   body capsules (bone indices of the body skeleton) the cloth collides with
       force   (fx, fy, fz) default force of the first notify if any )

The SimulationMesh itself is a mesh-like object: header with int 7 @42, NumPoints @46, Materials @51; its face list is `cidx 3F + 3F u16`
and its vertices are `cidx NP + NP x 32 bytes (pos 3f, normal 3f, uv 2f)`. The physics lives in the SimulationData object whose
SimulationPoints equal the mesh vertices: Anchors, WidthNodesCounts, SpringConstraintData (one SimulationSpringConstraint per particle with
NeighbourIndex[8] / DistAtRest[8]) and CollisionObjects (SimulationCylinderCollision: bones A-B + radius).
"""
import os, sys, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2pkg
from l2pkg import Reader
from essence_tex import read_props


def _cidx_list(raw, fmt):
    r = Reader(raw); n = r.cidx(); out = []
    for _ in range(n):
        if fmt == 'i': out.append(r.i32())
        elif fmt == 'f': out.append(r.f32())
        elif fmt == 'c': out.append(r.cidx())
        elif fmt == 'v': out.append((r.f32(), r.f32(), r.f32()))
    return out


def decode_mesh(pk, e):
    d = pk.read_obj(e)
    # vertices: cidx NP + NP x 32 bytes (the header count is not reliable for the ad00 meshes)
    verts = None
    for p in range(40, len(d) - 32 * 20):
        r = Reader(d); r.p = p
        try: n = r.cidx()
        except Exception: continue
        if n < 20 or n > 600 or r.p + 32 * n > len(d): continue
        q = r.p
        ok = True; rec = []
        for i in range(n):
            x, y, z, nx, ny, nz, u, v = struct.unpack_from('<8f', d, q + 32 * i)
            if not all(math.isfinite(c) for c in (x, y, z, nx, ny, nz, u, v)):
                ok = False; break
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            if not (abs(x) < 200 and abs(y) < 200 and abs(z) < 200 and 0.5 < ln < 1.5 and -1 < u < 3 and -1 < v < 3):
                ok = False; break
            rec.append((x, y, z, nx, ny, nz, u, v))
        if ok:
            verts = rec; break
    if verts is None:
        raise ValueError('vertices not found in ' + e['name'])
    NP = len(verts)
    # triangle list: cidx 3F then 3F u16 (all < NP)
    tris = None; best = (0, 0)
    for p in range(60, len(d) - 8):
        r = Reader(d); r.p = p
        try: n = r.cidx()
        except Exception: continue
        if n < 9 or n % 3 or r.p + 2 * n > len(d): continue
        v = struct.unpack_from('<%dH' % n, d, r.p)
        if max(v) < NP and len(set(v)) >= NP * 0.6 and (tris is None or (len(set(v)), n) > best):
            tris = [v[i:i + 3] for i in range(0, n, 3)]; best = (len(set(v)), n)
    if tris is None:
        raise ValueError('triangles not found in ' + e['name'])
    return verts, tris


def _spring_set(pk, ref_ids):
    """ref_ids: object references (export index, 1-based) of SimulationSpringConstraint objects, one per particle"""
    per = []
    for ref in ref_ids:
        e = pk.exports[ref - 1]
        props, _ = read_props(pk, pk.read_obj(e))
        nb = [props.get('NeighbourIndex' if k == 0 else 'NeighbourIndex[%d]' % k, -1) for k in range(8)]
        ds = [props.get('DistAtRest' if k == 0 else 'DistAtRest[%d]' % k, 0.0) for k in range(8)]
        per.append(list(zip(nb, ds)))
    return per


def load(path, mesh_name, pk=None):
    pk = pk or l2pkg.Package(path)
    me = next(e for e in pk.exports if pk.cls_name(e['cls']) == 'SimulationMesh' and e['name'].lower() == mesh_name.lower())
    verts, tris = decode_mesh(pk, me)
    NP = len(verts)
    # SimulationData with the same points (index by index), or - for the meshes that duplicate vertices along the UV seams (the clan
    # Hsm_ad00 one: 98 render vertices for 84 simulated points) - whose points are all positions of the mesh
    best = None; groups = None
    for e in pk.exports:
        if pk.cls_name(e['cls']) != 'SimulationData':
            continue
        props, _ = read_props(pk, pk.read_obj(e))
        raw = props.get('SimulationPoints')
        if not raw:
            continue
        pts = _cidx_list(bytes.fromhex(raw), 'v') if isinstance(raw, str) else None
        if pts and len(pts) == NP and all(abs(pts[i][0] - verts[i][0]) < 1e-3 and abs(pts[i][1] - verts[i][1]) < 1e-3 and abs(pts[i][2] - verts[i][2]) < 1e-3 for i in range(NP)):
            best = (e, props); groups = [[i] for i in range(NP)]; break
    if best is None:
        key = lambda p: (round(p[0] * 500), round(p[1] * 500), round(p[2] * 500))
        bypos = {}
        for i, v in enumerate(verts):
            bypos.setdefault(key(v), []).append(i)
        for e in pk.exports:
            if pk.cls_name(e['cls']) != 'SimulationData':
                continue
            props, _ = read_props(pk, pk.read_obj(e))
            raw = props.get('SimulationPoints')
            if not raw:
                continue
            pts = _cidx_list(bytes.fromhex(raw), 'v') if isinstance(raw, str) else None
            if not pts or len(pts) >= NP or len(pts) < NP * 0.6:
                continue
            gr = [bypos.get(key(p)) for p in pts]
            if all(gr) and sum(len(g) for g in gr) == NP and len({g[0] for g in gr}) == len(gr):
                best = (e, props); groups = gr; break
    if best is None:
        raise ValueError('no SimulationData matches the vertices of ' + mesh_name)
    sd, props = best
    NS = len(groups)                                       # simulated points
    anchors_s = _cidx_list(bytes.fromhex(props['Anchors']), 'i')
    widths = _cidx_list(bytes.fromhex(props['WidthNodesCounts']), 'i')
    refs = _cidx_list(bytes.fromhex(props['SpringConstraintData']), 'c')
    cols = _cidx_list(bytes.fromhex(props['CollisionObjects']), 'c') if props.get('CollisionObjects') else []
    sens_s = _cidx_list(bytes.fromhex(props['ForceSensitiveness']), 'f') if props.get('ForceSensitiveness') else [1.0] * NS
    spr_s = {}
    for i, lst in enumerate(_spring_set(pk, refs)):
        for j, dist in lst:
            if j < 0 or j >= NS or j == i:
                continue
            spr_s.setdefault((min(i, j), max(i, j)), dist)
    # expand to the render vertices: every simulated point owns its vertex group (seam duplicates are welded with rest-length 0 springs)
    sens = [1.0] * NP; anchors = []; springs = []
    for g, members in enumerate(groups):
        for v in members:
            sens[v] = sens_s[g] if g < len(sens_s) else 1.0
        if g in anchors_s or (g < len(anchors_s) and False):
            pass
    aset = set(anchors_s)
    for g in sorted(aset):
        anchors += groups[g]
    for (i, j), dist in sorted(spr_s.items()):
        for a_ in groups[i]:
            for b_ in groups[j]:
                springs.append((min(a_, b_), max(a_, b_), dist))
    welds = 0
    for members in groups:
        for k in range(1, len(members)):
            springs.append((members[0], members[k], 0.0)); welds += 1
    caps = []
    for ref in cols:
        props_c, _ = read_props(pk, pk.read_obj(pk.exports[ref - 1]))
        caps.append((props_c.get('BoneIndexA', 0), props_c.get('BoneIndexB', 0), props_c.get('Radius', 0.0), bool(props_c.get('SphereA', True)), bool(props_c.get('SphereB', True))))
    return dict(name=me['name'], data=sd['name'], verts=verts, tris=tris, anchors=anchors, width=(widths[0] if widths else 0), rows=len(widths),
                springs=springs, capsules=caps, sens=sens, simpoints=NS, welds=welds)


if __name__ == '__main__':
    root = os.path.join(os.environ.get('L2_ESSENCE_ROOT', ''), 'Animations')
    for body in ('MDarkElf', 'FElf'):
        for nm in ('Hsm_ad11', 'Rsm_ad11', 'Lsm_ad11'):
            try:
                m = load(root + '\\%sSimulation.uix' % body, '%s_%s' % (body, nm))
            except Exception as ex:
                print(body, nm, '->', ex); continue
            zs = [v[2] for v in m['verts']]; ys = [v[1] for v in m['verts']]
            print('%s_%s: %d verts, %d tris, grid %dx%d, anchors %s, %d springs (rest %.2f..%.2f), capsules %s | z %.1f..%.1f y %.1f..%.1f | data %s' % (
                body, nm, len(m['verts']), len(m['tris']), m['width'], m['rows'], m['anchors'], len(m['springs']), min(r for _, _, r in m['springs']), max(r for _, _, r in m['springs']),
                m['capsules'], min(zs), max(zs), min(ys), max(ys), m['data']))
