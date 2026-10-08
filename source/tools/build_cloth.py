#!/usr/bin/env python3
"""Pack the Essence standard-cloak cloth (SimulationMesh sets for the 14 bodies + the cloth textures of the official items) into essence_cloth.bin.

  python build_cloth.py <out.bin> [--max-looks 57] [--tex-top 512]

Looks come from the decoded Essence Armorgrp/ItemName (data\\essence_dat): one look per distinct (collar texture, cloth texture) pair of the MDarkElf column.
Layout (little endian):
   'ECL2' | u32 nTex | u32 nSets | u32 nCollars | u32 nLooks
   per texture: cstr name | u32 format | u32 w | u32 h | u32 levels | per level: u32 size, bytes          (same as ECP1)
   per set    : cstr body | cstr kind | u32 np | u32 width | u32 nt | u32 nAnch | u32 nSpr | u32 nCap
                np x 3f pos | np x 2f uv | np x f32 sens | nt x 3 u16 | nAnch x u16 | nSpr x (u16 i, u16 j, f32 rest) | nCap x (cstr boneA, cstr boneB, f32 radius) | i32 sec1
                (kind 'H' = Hsm_ad11 heavy, 'R' = Rsm_ad11 robe, 'C' = Hsm_ad00 clan cloak; sec1 = first triangle of the crest window section, -1 = none)
   per collar : cstr body | cstr family | u32 nv | u32 nt | nv x (3f pos, 2f uv) | nt x 3 u16        (rigid <Body>_<family> skeletal mesh, pinned like the anchors)
   per look   : cstr name | u8 kind | u32 clothTexture | u32 itemId | u32 collarTexture (0xFFFFFFFF none) | u32 crestTexture (0xFFFFFFFF none) | cstr collarFamily
"""
import os, sys, re, struct, argparse, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import io
from PIL import Image
import l2pkg
from l2pkg import Reader
import essence_sim as S
import essence_collar as CO
import fx_program as FX
from build_capes import fx_texture_index, pack_fx
import essence_cloaks as E
from essence_dxt import TexPackage, decode_dxt
from build_capes import texture_levels, cstr, find_ci

ROOT = os.environ.get('L2_ESSENCE_ROOT', '')
BODIES = ['MFighter', 'FFighter', 'MMagic', 'FMagic', 'MElf', 'FElf', 'MDarkElf', 'FDarkElf', 'MDwarf', 'FDwarf', 'MOrc', 'FOrc', 'MShaman', 'FShaman']
ANIM_PKGS = ['Fighter', 'Magic', 'Elf', 'DarkElf', 'Dwarf', 'Orc', 'Shaman']
DAT = os.path.join(os.environ.get('CLOAK_DATA', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data')), 'essence_dat') + os.sep
SKIP_NAME = re.compile(r'(?i)NPC|Tainted|Hussar|Wing$|Dynasty')


def body_bone_names(body):
    for pn in ANIM_PKGS:
        p = os.path.join(ROOT, 'Animations', pn + '.ukx')
        try:
            pk = l2pkg.Package(p)
        except Exception:
            continue
        e = find_ci(pk, 'MeshAnimation', body + '_anim')
        if e is None:
            continue
        d = pk.read_obj(e); r = Reader(d); r.p = 5; nb = r.cidx(); out = []
        for _ in range(nb):
            ni = r.cidx(); r.u32(); r.i32(); out.append(pk.names[ni])
        return out
    return None


def collect_looks(max_looks):
    items = E.parse_armorgrp(DAT + 'Armorgrp.txt'); names = E.parse_names(DAT + 'ItemName-eu.txt')
    looks = collections.OrderedDict()
    for c in sorted(E.cloaks(items), key=lambda c: c['id']):
        p = c['per'].get('MDarkElf')
        if not p:
            continue
        ms = [x for x in p if x.split('.')[0] in ('DarkElf', 'MDarkElfSimulation', 'LineageAccessory3', 'BranchDarkElf')]
        tx = [x for x in p if x not in ms and not re.fullmatch(r'\d+', x)]
        if len(ms) < 6 or len(tx) < 2:
            continue                                         # not a standard cloak (animated mantle / aegis / ...)
        nm = names.get(c['id'], '?')
        if SKIP_NAME.search(nm) or tx[1].split('.')[0].lower() in ('dropitemstex', 'linagetranstex', 'lineagetranstex'):
            continue
        if not any(t in tx[1] for t in ('_t01', '_t00', 'faction')):
            continue
        key = (tx[0], tx[1])
        if key not in looks:
            sims = [x.split('.')[-1] for x in ms if re.search(r'_[HRL]sm_', x, re.I)]
            rms = [x.split('.')[-1] for x in ms if re.search(r'_[HRL]rm_', x, re.I)]
            if any(re.search(r'_Hsm_ad00', s, re.I) for s in sims): kind = 'C'
            elif sims and all(re.search(r'_Rsm_', s, re.I) for s in sims): kind = 'R'
            else: kind = 'R' if re.search(r'(^|[._])R_', tx[1]) else 'H'
            pick = [m for m in rms if re.search(r'_Rrm_' if kind == 'R' else r'_Hrm_', m, re.I)] or rms
            fam = None
            if pick:
                nm0 = pick[0]; pre = 'mdarkelf_'
                fam = nm0[len(pre):] if nm0.lower().startswith(pre) else None
            crest = next((x for x in tx if re.search(r'_t02', x, re.I)), None)
            looks[key] = dict(name=nm, id=c['id'], icon=c['icon'], tex=tx[1], collar=tx[0], crest=crest, kind=kind, family=fam, ids=[])
        looks[key]['ids'].append(c['id'])
    return list(looks.values())[:max_looks] if max_looks else list(looks.values())


def resolve_texture(ref, cache):
    """'Mantle.knig.H_knig_t01' -> (fmt, w, h, levels) of the diffuse texture of that material"""
    parts = ref.split('.')
    pkgname, name = parts[0], parts[-1]; group = parts[1] if len(parts) == 3 else ''
    p = os.path.join(ROOT, 'SysTextures', pkgname + '.utx')
    if not os.path.exists(p):
        return None
    if p not in cache:
        cache[p] = TexPackage(p)
    tp = cache[p]
    try:
        mt = tp.material(group, name)
    except Exception:
        # a bare texture
        e = tp.find(group, name) if group else None
        if e is None or tp.pk.cls_name(e['cls']) != 'Texture':
            return None
        te = e
        fmt, w, h, levels = texture_levels(tp.pk, te)
        return te['name'], fmt, w, h, levels
    te = next(e for e in tp.pk.exports if e['name'] == mt['texture'] and tp.pk.cls_name(e['cls']) == 'Texture')
    fmt, w, h, levels = texture_levels(tp.pk, te)
    return te['name'], fmt, w, h, levels


def resolve_crest(ref, cache):
    """'Mantleguild.guild2.H_grow_1_t02' (a UserDefinableMaterial: crest background + the clan crest) -> the background texture"""
    if not ref:
        return None
    parts = ref.split('.'); pkgname, name = parts[0], parts[-1]
    p = os.path.join(ROOT, 'SysTextures', pkgname + '.utx')
    if not os.path.exists(p):
        return None
    if p not in cache:
        cache[p] = TexPackage(p)
    tp = cache[p]
    for nm in (name + '_bg', name):
        te = next((e for e in tp.pk.exports if e['name'].lower() == nm.lower() and tp.pk.cls_name(e['cls']) == 'Texture'), None)
        if te is not None:
            fmt, w, h, levels = texture_levels(tp.pk, te)
            return te['name'], fmt, w, h, levels
    return None


def solid_texture(name, ref):
    """no crest background texture (the ally clan cloaks): a 4x4 DXT1 block of the average fabric colour of the cloth texture around the window"""
    tname, fmt, w, h, levels = ref
    c = window_ring_color(ref)
    if c is None:
        img = decode_dxt(levels[0], w, h, fmt)
        reg = img[int(h * 0.15):int(h * 0.45), int(w * 0.30):int(w * 0.70)].reshape(-1, 4)
        reg = reg[reg[:, 3] >= 160]
        c = reg[:, :3].mean(axis=0) if len(reg) else np.array([90, 90, 90.])
    r, g, b = [int(round(x)) for x in c]
    c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    blk = struct.pack('<HHI', c565, c565, 0)
    return (name, 3, 4, 4, [blk])


_WIN = {}
def window_ring_color(cloth_ref):
    """mean colour of the cloth texture right around the crest window (cloth triangles that share a vertex position with the window ones)"""
    if 'm' not in _WIN:
        pkp = os.path.join(ROOT, 'Animations', 'MDarkElfSimulation.uix')
        _WIN['m'] = S.load(pkp, 'MDarkElf_Hsm_ad00')
    m = _WIN['m']; V, T = m['verts'], m['tris']
    s1 = crest_section_start(m)
    key = lambda v: (round(v[0] * 400), round(v[1] * 400), round(v[2] * 400))
    win = {key(V[i]) for tr in T[s1:] for i in tr}
    tname, fmt, w, h, levels = cloth_ref
    img = decode_dxt(levels[0], w, h, fmt)
    px = []
    for tr in T[:s1]:
        if any(key(V[i]) in win for i in tr):
            for (a, b, c) in ((1 / 3, 1 / 3, 1 / 3), (0.6, 0.2, 0.2), (0.2, 0.6, 0.2), (0.2, 0.2, 0.6)):
                u = (a * V[tr[0]][6] + b * V[tr[1]][6] + c * V[tr[2]][6]); v = (a * V[tr[0]][7] + b * V[tr[1]][7] + c * V[tr[2]][7])
                x = min(w - 1, max(0, int(u * w))); y = min(h - 1, max(0, int(v * h)))
                p = img[y, x]
                if p[3] >= 160 and int(p[0]) + int(p[1]) + int(p[2]) > 60: px.append(p[:3])
    return np.array(px, float).mean(axis=0) if px else None


def _dxt1_levels(img):
    """RGBA uint8 image -> (w, h, [DXT1 bytes per level down to 4x4]) through Pillow's DDS writer"""
    im = Image.fromarray(img[:, :, :3], 'RGB'); w, h = im.size; out = []
    while True:
        b = io.BytesIO(); im.save(b, 'DDS', pixel_format='DXT1'); out.append(b.getvalue()[128:])
        if w <= 4 or h <= 4: break
        w //= 2; h //= 2; im = im.resize((w, h), Image.LANCZOS)
    return out


def tint_bg(bg_ref, ring):
    """crest background texture re-coloured so that its mean matches the cloth around the window (the Essence bg is tuned for the crest on top)"""
    tname, fmt, w, h, levels = bg_ref
    img = decode_dxt(levels[0], w, h, fmt).copy()
    mean = img[:, :, :3].reshape(-1, 3).astype(float).mean(axis=0)
    gain = np.clip(ring / np.maximum(mean, 1.0), 0.5, 2.0)
    img[:, :, :3] = np.clip(img[:, :, :3].astype(float) * gain, 0, 255).astype(np.uint8)
    return (tname + '_tint', 3, w, h, _dxt1_levels(img))


_COLLAR_PKGS = {}
def find_collar(body, family):
    """decoded collar mesh <body>_<family> (case-insensitive) searched in the body packages and LineageAccessory3; None if absent"""
    want = (body + '_' + family).lower()
    for pn in ANIM_PKGS + ['LineageAccessory3']:
        if pn not in _COLLAR_PKGS:
            try:
                pk = l2pkg.Package(os.path.join(ROOT, 'Animations', pn + '.ukx'))
            except Exception:
                pk = None
            _COLLAR_PKGS[pn] = (pk, {e['name'].lower(): e for e in pk.exports if pk.cls_name(e['cls']) == 'SkeletalMesh'} if pk else {})
        pk, idx = _COLLAR_PKGS[pn]
        if want in idx:
            try:
                return CO.decode(pk, idx[want])
            except Exception as ex:
                print('  collar %s: %s' % (want, ex)); return None
    return None


def crest_section_start(m):
    """first triangle of the crest window section: the tail of the face list whose UV density is >> 3D density (it maps the whole crest texture)"""
    V, T = m['verts'], m['tris']
    def ratio(t):
        a, b, c = [V[i] for i in t]
        uva = abs((b[6] - a[6]) * (c[7] - a[7]) - (c[6] - a[6]) * (b[7] - a[7])) / 2
        u = [b[k] - a[k] for k in range(3)]; v = [c[k] - a[k] for k in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        return uva / max((n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 / 2, 1e-9) * 1000
    k = len(T)
    while k > 0 and ratio(T[k - 1]) > 5.0:
        k -= 1
    return k if k < len(T) else -1


def trim_levels(w, h, levels, top):
    while w > top and len(levels) > 1:
        levels = levels[1:]; w >>= 1; h >>= 1
    return w, h, levels



_FXCACHE = {}
def fx_for_ref(ref, tex_blobs, fx_textures, fx_programs):
    """effect layers of the material `ref` ('Pkg.group.name'); textures are appended to tex_blobs; programs keyed by the lowercase base texture name"""
    if not ref:
        return
    parts = ref.split('.'); pkgname, name = parts[0], parts[-1]; group = parts[1] if len(parts) == 3 else ''
    p = FX._utx_path(pkgname)
    if not p:
        return
    if p not in _FXCACHE:
        _FXCACHE[p] = TexPackage(p)
    tp = _FXCACHE[p]
    try:
        base, layers = FX.extract(tp, group, name)
    except Exception as ex:
        print('   fx extract failed for %s: %s' % (ref, ex)); return
    if not base or not layers or base.lower() in fx_programs:
        return
    prog = []
    for l in layers:
        try:
            texs = [dict(tx, index=fx_texture_index(tx, fx_textures, tex_blobs)) for tx in l['texs']]
        except StopIteration:
            continue
        prog.append(dict(kind=l['kind'], texs=texs))
    if prog:
        fx_programs[base.lower()] = prog
        print('   fx %-34s %s' % (base, FX.describe(layers)))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('out'); ap.add_argument('--max-looks', type=int, default=57); ap.add_argument('--tex-top', type=int, default=512)
    a = ap.parse_args()
    looks = collect_looks(a.max_looks)
    print(len(looks), 'looks')
    cache = {}; tex_blobs = []; tex_index = {}; out_looks = []; fx_textures = {}; fx_programs = {}

    def add_tex(r, top):
        tname, fmt, w, h, levels = r
        w, h, levels = trim_levels(w, h, levels, top)
        key = (tname, w)
        if key not in tex_index:
            tex_index[key] = len(tex_blobs)
            tex_blobs.append(cstr(tname) + struct.pack('<IIII', fmt, w, h, len(levels)) + b''.join(struct.pack('<I', len(l)) + l for l in levels))
        return tex_index[key], w, h, fmt

    for lk in looks:
        r = resolve_texture(lk['tex'], cache)
        if r is None:
            print('  sem textura:', lk['name'], lk['tex']); continue
        ti, w, h, fmt = add_tex(r, a.tex_top)
        rc = resolve_texture(lk['collar'], cache)
        ci = add_tex(rc, 256)[0] if rc else 0xFFFFFFFF
        rcr = resolve_crest(lk['crest'], cache) if lk['kind'] == 'C' else None
        if lk['kind'] == 'C' and rcr is None:
            rcr = solid_texture('crest_solid_' + lk['tex'].split('.')[-1], r)
        elif lk['kind'] == 'C':
            ring = window_ring_color(r)
            if ring is not None: rcr = tint_bg(rcr, ring)
        cri = add_tex(rcr, 256)[0] if rcr else 0xFFFFFFFF
        out_looks.append((lk, ti, ci, cri))
        fx_for_ref(lk['tex'], tex_blobs, fx_textures, fx_programs); fx_for_ref(lk['collar'], tex_blobs, fx_textures, fx_programs)
        print('  %-36s ids %-22s kind %s tex %-30s %dx%d fmt %d | collar %s %s | crest %s' % (lk['name'][:36], ','.join(str(i) for i in lk['ids'][:3]), lk['kind'], r[0][:30], w, h, fmt, lk['family'], 'ok' if ci != 0xFFFFFFFF else 'SEM TEX', 'ok' if cri != 0xFFFFFFFF else ('-' if lk['kind'] != 'C' else 'SEM TEX')))
    # cloth sets
    set_blobs = []
    for body in BODIES:
        bones = body_bone_names(body)
        pkp = os.path.join(ROOT, 'Animations', body + 'Simulation.uix')
        pk = l2pkg.Package(pkp)
        for kind, suffix in (('H', 'Hsm_ad11'), ('R', 'Rsm_ad11'), ('C', 'Hsm_ad00')):
            try:
                m = S.load(pkp, '%s_%s' % (body, suffix), pk)
            except Exception as ex:
                print('  set %s %s: %s' % (body, kind, ex)); continue
            caps = []
            for ia, ib, rad, _, _ in m['capsules']:
                if bones is None or ia >= len(bones) or ib >= len(bones):
                    continue
                caps.append((bones[ia], bones[ib], rad))
            sec1 = crest_section_start(m) if kind == 'C' else -1
            blob = bytearray(cstr(body) + cstr(kind))
            NP = len(m['verts'])
            blob += struct.pack('<IIIIII', NP, m['width'], len(m['tris']), len(m['anchors']), len(m['springs']), len(caps))
            for v in m['verts']: blob += struct.pack('<3f', v[0], v[1], v[2])
            for v in m['verts']: blob += struct.pack('<2f', v[6], v[7])
            for k in range(NP): blob += struct.pack('<f', m['sens'][k] if k < len(m['sens']) else 1.0)
            for tr in m['tris']: blob += struct.pack('<3H', *tr)
            for an in m['anchors']: blob += struct.pack('<H', an)
            for i, j, rest in m['springs']: blob += struct.pack('<HHf', i, j, rest)
            for na, nb_, rad in caps: blob += cstr(na) + cstr(nb_) + struct.pack('<f', rad)
            blob += struct.pack('<i', sec1)
            set_blobs.append(bytes(blob))
            print('  set %-9s %s: %d particles, %d tris (crest from %d), %d springs, %d capsules %s' % (body, kind, NP, len(m['tris']), sec1, len(m['springs']), len(caps), [c[:2] for c in caps][:2]))
    # rigid collars (one per body and family used by a look)
    fams = sorted({lk['family'] for lk, _, _, _ in out_looks if lk['family']})
    coll_blobs = []; missing = []
    for body in BODIES:
        for fam in fams:
            m = find_collar(body, fam)
            if m is None:
                missing.append((body, fam)); continue
            W = m['wedges']; P = m['points']
            blob = bytearray(cstr(body) + cstr(fam) + struct.pack('<II', len(W), len(m['faces'])))
            for pt, u, v in W: blob += struct.pack('<3f2f', P[pt][0], P[pt][1], P[pt][2], u, v)
            for fc in m['faces']: blob += struct.pack('<3H', *fc)
            coll_blobs.append(bytes(blob))
    print('  collars: %d (body x family), %d families %s, %d missing %s' % (len(coll_blobs), len(fams), fams, len(missing), missing[:6]))
    with open(a.out, 'wb') as f:
        f.write(b'ECL2' + struct.pack('<IIII', len(tex_blobs), len(set_blobs), len(coll_blobs), len(out_looks)))
        for tb_ in tex_blobs: f.write(tb_)
        for s in set_blobs: f.write(s)
        for cb in coll_blobs: f.write(cb)
        for lk, ti, ci, cri in out_looks:
            f.write(cstr(lk['name']) + lk['kind'].encode('ascii') + struct.pack('<IIII', ti, lk['id'], ci, cri) + cstr(lk['family'] or ''))
        f.write(pack_fx(fx_programs))
    print('wrote', a.out, '%.1f MB' % (os.path.getsize(a.out) / 1e6), '| textures', len(tex_blobs), 'sets', len(set_blobs), 'collars', len(coll_blobs), 'looks', len(out_looks))


if __name__ == '__main__':
    main()
