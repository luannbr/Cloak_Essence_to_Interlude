"""Pack Essence mantles (mesh + skeleton + baked animations + DXT textures) into one binary the cloakhook loads.
   python build_capes.py <mantles.ukx> <tex.utx> <out.bin> --bodies MFighter,MDarkElf --designs 0

Everything is little-endian. Quaternions are stored already CONJUGATED (the verified convention), as (x, y, z, w).
File layout:
   char[4] 'ECP1' | u32 nBodies | u32 nTextures | u32 nMantles
   per body : cstr name | u32 nBones | per bone: cstr name, i32 parent | u32 nSeqs
              per seq : cstr name(lowercase) | u32 frames | f32 rate | per bone: u8 flags (bit0 quats per frame, bit1 positions per frame, 0xFF bone absent),
                        static quat 4f, static pos 3f, [frames x 4 x i16 quats /32767], [frames x 3 f32 positions]
   per texture : cstr name | u32 format | u32 w | u32 h | u32 levels | per level: u32 size, bytes
   per mantle  : cstr body | u32 design | cstr meshName | u32 nBones | per bone: cstr name, i32 parent, 4f quat(conj), 3f pos
                 | u32 nPoints | nPoints x 3f | nPoints x (4 x u8 bone, 4 x f32 weight) | u32 nWedges | nWedges x (u16 point, f32 u, f32 v)
                 | u32 nFaces | nFaces x 3 u16 | u32 nSections | per section: u32 first, u32 count, u32 material | u32 nMaterials
                 | per material: u32 textureIndex, u32 alphaTest, u32 alphaRef, u32 twoSided
"""
import os, sys, struct, argparse, math, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence import Package, parse_mesh, parse_anim
from essence_tex import read_props
from essence_dxt import TexPackage, FMT_DXT1, FMT_DXT3, FMT_DXT5
import fx_program as FX


def cstr(s):
    return s.encode('ascii', 'replace') + b'\0'


def texture_levels(pk, e):
    d = pk.read_obj(e)
    props, pend = read_props(pk, d)
    w, h, fmt = props['USize'], props['VSize'], props['Format']
    ub, vb = props.get('UBits', 0), props.get('VBits', 0)
    levels = []
    start = pend
    for lv in range(0, 12):
        lw, lh = max(1, w >> lv), max(1, h >> lv)
        if lw < 4 or lh < 4:
            break
        size = (lw * lh // 2) if fmt == FMT_DXT1 else (lw * lh)
        pat = struct.pack('<ii', lw, lh) + bytes([max(0, ub - lv), max(0, vb - lv)])
        foot = d.find(pat, start)
        if foot < 0 or foot - size < start:
            break
        levels.append(d[foot - size:foot])
        start = foot + len(pat)
    if not levels:
        raise ValueError('no mip levels in ' + e['name'])
    return fmt, w, h, levels


def resample_quats(tr, frames):
    q = np.array(tr['quat'], dtype=np.float64)
    t = np.array(tr['times'], dtype=np.float64)
    if len(q) == 1:
        return None, q[0]
    out = np.zeros((frames, 4))
    for f in range(frames):
        if f <= t[0]:
            out[f] = q[0]
        elif f >= t[-1]:
            out[f] = q[-1]
        else:
            k = int(np.searchsorted(t, f, side='right') - 1)
            span = t[k + 1] - t[k]
            a = (f - t[k]) / span if span > 1e-9 else 0.0
            q0, q1 = q[k], q[k + 1]
            if np.dot(q0, q1) < 0:
                q1 = -q1
            v = q0 * (1 - a) + q1 * a
            out[f] = v / np.linalg.norm(v)
    return out, None


def resample_pos(tr, frames):
    p = np.array(tr['pos'], dtype=np.float64)
    if len(p) <= 1:
        return None, (p[0] if len(p) else np.zeros(3))
    t = np.array(tr['times'][:len(p)] if len(tr['times']) >= len(p) else np.linspace(0, frames - 1, len(p)), dtype=np.float64)
    out = np.zeros((frames, 3))
    for f in range(frames):
        out[f] = [np.interp(f, t, p[:, k]) for k in range(3)]
    return out, None


def conj(q, bone=1):
    # verified on idle/sit/run poses: the ROOT bone is stored with the opposite sign convention of its children (ActorX quirk)
    return np.array([q[0], q[1], q[2], q[3]]) if bone == 0 else np.array([-q[0], -q[1], -q[2], q[3]])


def pack_body(name, anim):
    out = bytearray()
    nb = len(anim['bones'])
    out += cstr(name) + struct.pack('<I', nb)
    for bn, par in anim['bones']:
        out += cstr(bn) + struct.pack('<i', par)
    out += struct.pack('<I', len(anim['seqs']))
    stats = dict(quat_full=0, quat_static=0, pos_full=0)
    for s, ch in zip(anim['seqs'], anim['chunks']):
        frames = ch['frames']
        out += cstr(s['name'].lower()) + struct.pack('<If', frames, float(s['rate']))
        by_bone = {tr['bone']: tr for tr in ch['tracks']}
        for b in range(nb):
            tr = by_bone.get(b)
            if tr is None:
                out += bytes([0xFF]); continue
            qs, q0 = resample_quats(tr, frames)
            ps, p0 = resample_pos(tr, frames)
            flags = (1 if qs is not None else 0) | (2 if ps is not None else 0)
            sq = conj(qs[0] if qs is not None else q0, b)
            sp = ps[0] if ps is not None else p0
            out += bytes([flags]) + struct.pack('<4f3f', *sq, *sp)
            if qs is not None:
                arr = np.array([conj(q, b) for q in qs])
                out += np.round(arr * 32767.0).astype('<i2').tobytes()
                stats['quat_full'] += 1
            else:
                stats['quat_static'] += 1
            if ps is not None:
                out += ps.astype('<f4').tobytes(); stats['pos_full'] += 1
    return bytes(out), stats


def pack_mantle(body, design, mesh, tex_index):
    out = bytearray()
    out += cstr(body) + struct.pack('<I', design) + cstr(mesh['name'])
    out += struct.pack('<I', len(mesh['bones']))
    for bi_, (bn, par, q, pos) in enumerate(mesh['bones']):
        out += cstr(bn) + struct.pack('<i', par) + struct.pack('<4f3f', *conj(np.array(q), bi_), *pos)
    P = mesh['points']; NP = len(P)
    out += struct.pack('<I', NP)
    out += np.array(P, dtype='<f4').tobytes()
    per = [[] for _ in range(NP)]
    for w, pt, bi in mesh['infl']:
        per[pt].append((w, bi))
    for pt in range(NP):
        inf = sorted(per[pt], reverse=True)[:4]
        tot = sum(w for w, _ in inf) or 1.0
        inf = [(w / tot, b) for w, b in inf] + [(0.0, 0)] * (4 - len(inf))
        out += bytes(b for _, b in inf) + struct.pack('<4f', *[w for w, _ in inf])
    W = mesh['wedges']
    out += struct.pack('<I', len(W))
    for pt, u, v in W:
        out += struct.pack('<Hff', pt, u, v)
    F = mesh['faces']
    out += struct.pack('<I', len(F))
    out += np.array(F, dtype='<u2').tobytes()
    secs = mesh.get('sections') or [(0, len(F))]
    out += struct.pack('<I', len(secs))
    for si, (first, cnt) in enumerate(secs):
        out += struct.pack('<III', first, cnt, min(si, len(mesh['materials']) - 1))
    out += struct.pack('<I', len(mesh['materials']))
    for mt in mesh['_mats']:
        out += struct.pack('<IIII', tex_index[mt['texture']], 1 if mt['alpha_test'] else 0, mt['alpha_ref'], 1 if mt['two_sided'] else 0)
    return bytes(out)


# design id -> (mesh name template, colour-variant swap, source package key, animation set template, animation key template)
#   {b} = body, {s} = sex letter (M|F) of the body. The animation key is what the hook looks up (EssFind): the body for the NewMantles,
#   <Body>_aegis, <Body>_valakas, and the body-less 'JDK' (one mesh + one animation set shared by every body).
DESIGNS = {i: ('%s_NewMantle%02d_m_ad00' % ('{b}', i), None, 'mantles', '{b}_cape_anim', '{b}') for i in range(8)}
DESIGNS[8] = ('{b}_NewMantleRus2024_m_ad00', None, 'mantles', '{b}_cape_anim', '{b}')
DESIGNS[9] = ('{b}_NewMantle00_m_ad00', ('NewMantle00_', 'NewMantle00_red_'), 'mantles', '{b}_cape_anim', '{b}')      # same mesh, red colourway
DESIGNS[10] = ('{b}_aegis_cloak_m_ad00', None, 'aegis', '{b}_aegis_cloak_anim', '{b}_aegis')                          # Woeful Sword Eigis's Cloak: own skeleton + a single idle loop
DESIGNS[11] = ('{b}_valakas_Wing_m_ad00', None, 'cust21', '{b}_valacas_Wing_Anim', '{b}_valakas')                     # dragon wings (Wait / Flap)
DESIGNS[12] = ('JDK_Cloak_m_ad00', None, 'cust16', 'JDK_Cloak_anim', 'JDK')                                           # Death Knight's Cloak R99 (bat wings), body-less
DESIGNS[13] = ('JDK_RankerCloak_m_ad00', None, 'cust16', 'JDK_Cloak_anim', 'JDK')                                     # Death Knight's Cloak R110 (red bat wings)
DESIGNS[14] = ('JDK_Ranker_NewMantle00_m_ad00', None, 'mantles', '{b}_cape_anim', '{b}')                              # Death Knight ranker mantle: golden shoulder wings + cape (JDK mesh on the body's cape animation)
DESIGNS[15] = ('FDwarf_MG_RankerMantle00_m_ad00', None, 'mantles', '{b}_cape_anim', '{b}')                           # official 82972: white/gold angel ribbons (FDwarf MG mesh on the body's cape animation)



def fx_texture_index(tinfo, textures, tex_blobs, top=512):
    """index of an effect texture in the pack (added on first use); non-DXT formats are converted to DXT5, big ones are trimmed to `top` px"""
    name = tinfo['name']
    if name in textures:
        return textures[name]
    pk = tinfo['tp'].pk
    te = next(e for e in pk.exports if e['name'] == name and pk.cls_name(e['cls']) == 'Texture')
    fmt, w, h, levels = texture_levels(pk, te)
    if fmt not in (3, 7, 8):
        from essence_dxt import texture_mip0
        import io
        from PIL import Image
        img = texture_mip0(pk, te); im = Image.fromarray(img, 'RGBA'); w, h = im.size; levels = []; fmt = 8
        while True:
            b = io.BytesIO(); im.save(b, 'DDS', pixel_format='DXT5'); levels.append(b.getvalue()[128:])
            if w <= 4 or h <= 4: break
            w //= 2; h //= 2; im = im.resize((w, h), Image.LANCZOS)
        w, h = img.shape[1], img.shape[0]
    while w > top and len(levels) > 1:
        levels = levels[1:]; w >>= 1; h >>= 1
    textures[name] = len(tex_blobs)
    tex_blobs.append(cstr(name) + struct.pack('<IIII', fmt, w, h, len(levels)) + b''.join(struct.pack('<I', len(l)) + l for l in levels))
    print('   fx texture %s fmt %d %dx%d levels %d' % (name, fmt, w, h, len(levels)))
    return textures[name]


def pack_fx(programs):
    """FXS1 section: u32 nProg | per program: cstr key | u8 nLayers | per layer: u8 kind (0 add, 1 lerp) | u8 nTex | per tex: u32 tex | u8 flags (1 env, 2 mask) | f32 panU | f32 panV"""
    out = bytearray(b'FXS1' + struct.pack('<I', len(programs)))
    for key, layers in programs.items():
        out += cstr(key) + struct.pack('<B', len(layers))
        for l in layers:
            out += struct.pack('<BB', 0 if l['kind'] == 'add' else 1, len(l['texs']))
            for tx in l['texs']:
                out += struct.pack('<IBff', tx['index'], (1 if tx['env'] else 0) | (2 if tx.get('mask') else 0), tx['panU'], tx['panV'])
    return bytes(out)

# the race-themed Ranker cloaks (LineageCustom16): rigid legacy meshes on the body's cape animation (root only); design 16
RANKER16 = {'MDarkElf': 'MDarkElf_Ranker_Cloak_m_ad00', 'FDarkElf': 'FDarkElf_Ranker_Cloak_m_ad00'}      # the other races' Ranker cloaks use other texture packages (not done)
DESIGNS[16] = ('{r16}', None, 'cust16', '{b}_cape_anim', '{b}')


def find_ci(pk, cls, name):
    for e in pk.exports:
        if e['name'].lower() == name.lower() and pk.cls_name(e['cls']) == cls:
            return e
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mantles_ukx'); ap.add_argument('out')
    ap.add_argument('--utx', action='append', required=True, help='texture packages, searched in order (repeat the option)')
    ap.add_argument('--aegis-ukx', default=None, help='LineageCustom8.ukx (design 10)')
    ap.add_argument('--src', action='append', default=[], help='extra source package key=path (cust16=LineageCustom16.ukx, cust21=LineageCustom21.ukx)')
    ap.add_argument('--bodies', required=True); ap.add_argument('--designs', default='0')
    a = ap.parse_args()
    pks = {'mantles': Package(a.mantles_ukx)}
    if a.aegis_ukx: pks['aegis'] = Package(a.aegis_ukx)
    for kv in a.src:
        k, p = kv.split('=', 1); pks[k] = Package(p)
    tps = [TexPackage(u) for u in a.utx]
    bodies = a.bodies.split(','); designs = [int(x) for x in a.designs.split(',')]
    body_blobs = []; mantles = []; textures = {}; tex_blobs = []; done_anims = set(); done_mantles = set(); fx_programs = {}
    for body in bodies:
        for d in designs:
            tmpl, swap, src, anim_tmpl, key_tmpl = DESIGNS[d]
            pk = pks.get(src)
            if pk is None:
                print('   source package for design %d not given' % d); continue
            if d == 16 and body not in RANKER16:
                continue
            key = key_tmpl.format(b=body, s=body[0])
            if (key, d) in done_mantles:
                continue
            if key not in done_anims:
                apk = pks['mantles'] if anim_tmpl.endswith('_cape_anim') else pk
                ae = find_ci(apk, 'MeshAnimation', anim_tmpl.format(b=body, s=body[0]))
                if ae is None:
                    print('no animation set for', key, '(', anim_tmpl.format(b=body, s=body[0]), ')'); continue
                anim = parse_anim(apk, ae)
                blob, st = pack_body(key, anim)
                body_blobs.append(blob); done_anims.add(key)
                print('body %-18s bones %d seqs %d  tracks: %s  -> %.1f MB' % (key, len(anim['bones']), len(anim['seqs']), st, len(blob) / 1e6))
            me = find_ci(pk, 'SkeletalMesh', tmpl.format(b=body, s=body[0], r16=RANKER16.get(body, '')))
            if me is None:
                print('   no mesh for', key, 'design', d); continue
            if d == 16:
                import essence_collar as _EC
                mesh = _EC.decode_rigid_cloak(pk, me)
            else:
                mesh = parse_mesh(pk, me)
            mats = []
            for ref in mesh['materials']:
                imp = pk.imports[-ref - 1]; group = pk.imports[-imp[2] - 1][3]; mname = imp[3]
                if swap:
                    group = swap[1].rstrip('_')
                    mname = mname.replace(swap[0], swap[1])
                mt = None; owner = None
                for tp in tps:
                    try:
                        mt = tp.material(group, mname); owner = tp; break
                    except Exception:
                        continue
                if mt is None:
                    raise SystemExit('material %s.%s not found in any texture package' % (group, mname))
                tname = mt['texture']
                if tname not in textures:
                    te = next(e for e in owner.pk.exports if e['name'] == tname and owner.pk.cls_name(e['cls']) == 'Texture')
                    fmt, w, h, levels = texture_levels(owner.pk, te)
                    textures[tname] = len(tex_blobs)
                    blob = cstr(tname) + struct.pack('<IIII', fmt, w, h, len(levels)) + b''.join(struct.pack('<I', len(l)) + l for l in levels)
                    tex_blobs.append(blob)
                    print('   texture %s fmt %d %dx%d levels %d' % (tname, fmt, w, h, len(levels)))
                mats.append(mt)
                fxkey = mt['texture'].lower()
                if fxkey not in fx_programs:
                    try:
                        _b, layers = FX.extract(owner, group, mname)
                    except Exception as ex:
                        print('   fx extract failed for %s.%s: %s' % (group, mname, ex)); layers = []
                    prog = []
                    for l in layers:
                        try:
                            texs = [dict(tx, index=fx_texture_index(tx, textures, tex_blobs)) for tx in l['texs']]
                        except StopIteration:
                            continue
                        prog.append(dict(kind=l['kind'], texs=texs))
                    fx_programs[fxkey] = prog
                    if prog: print('   fx %-34s %s' % (mt['texture'], FX.describe(layers)))
            mesh['_mats'] = mats
            mantles.append(pack_mantle(key, d, mesh, textures)); done_mantles.add((key, d))
            print('   mantle %s (design %d, key %s): %d points %d wedges %d faces sections %s' % (mesh['name'], d, key, len(mesh['points']), len(mesh['wedges']), len(mesh['faces']), mesh.get('sections')))
    with open(a.out, 'wb') as f:
        f.write(b'ECP1' + struct.pack('<III', len(body_blobs), len(tex_blobs), len(mantles)))
        for b in body_blobs: f.write(b)
        for t in tex_blobs: f.write(t)
        for m in mantles: f.write(m)
        f.write(pack_fx({k: v for k, v in fx_programs.items() if v}))
    print('wrote', a.out, os.path.getsize(a.out) / 1e6, 'MB')


if __name__ == '__main__':
    main()
