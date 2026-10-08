#!/usr/bin/env python3
"""Builds essence_fx.bin: the particle effects the Essence cloaks spawn (LineageEffect*.u emitters) in a form the hook can simulate.

  python build_fx.py <out.bin> [effect names...]

SpriteEmitter, MeshEmitter and VertMeshEmitter (only the `wing_high` wing mesh) are converted (beams and trails are skipped).
Layout (little endian):  'EFX3', u32 nTex, u32 nMesh, u32 nVMesh, u32 nEff
  tex   : cstr name, u32 fmt, w, h, levels, levels x (u32 size, data)               (same blob as the cloth pack)
  mesh  : cstr name, u32 nv, nt, i32 tex, nv x (x y z u v), nt x 3 x u16
  vmesh : cstr name, u32 nv, nf, nt, nt x 3 x u16, nv x (u v), nf x nv x (x y z)
  effect: cstr name, u32 nEmitters, emitters (see pack_emitter)
"""
import os, sys, struct, io
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2pkg, ue_props as U, essence_static as ES, essence_vertmesh as VM, fx_program as FX
from essence_dxt import TexPackage, texture_mip0
from build_capes import cstr, fx_texture_index

ROOT = os.environ.get('L2_ESSENCE_ROOT', '')
EFFECT_PKGS = ('LineageEffect.u', 'LineageEffect2.u', 'LineageEffect3.u')
DEFAULT = ['d_cloth_deco_a', 'd_cloth_deco_b', 'd_cloth_deco_c', 'd_cloth_deco_d', 'd_cloth_deco_e', 'hero_cloth_deco', 'h_worldSiege_cloth_deco', 'h_worldSiege_clothB_deco',
           'h_worldSiege_clothC_deco', 'h_new_clothB_deco', 'h_new_clothC_deco', 'wing_high_cloth_deco', 'v_disron_cloth_deco', 'b_anta_cloth_deco', 'b_vala_cloth_deco',
           'b_lind_cloth_deco', 'b_fafu_cloth_deco', 'h_rankercape_01_deco']

_pk = {}
_tp = {}


def package(name):
    if name not in _pk:
        sub = 'system' if name.endswith('.u') else 'StaticMeshes'
        _pk[name] = l2pkg.Package(os.path.join(ROOT, sub, name))
    return _pk[name]


def texpkg(name):
    p = FX._utx_path(name)
    if not p:
        return None
    if p not in _tp:
        _tp[p] = TexPackage(p)
    return _tp[p]


def find_class(name):
    for f in EFFECT_PKGS:
        pk = package(f)
        for i, e in enumerate(pk.exports):
            if e['name'] == name and pk.cls_name(e['cls']) == 'Class':
                return pk, i + 1
    raise KeyError(name)


def group_of(pk, e):
    names = []
    o = e['pkg']
    while o > 0:
        names.append(pk.exports[o - 1]['name'])
        o = pk.exports[o - 1]['pkg']
    return list(reversed(names))


def find_static(pkgname, group, name):
    pk = package(pkgname + '.usx')
    best = None
    for e in pk.exports:
        if e['name'].lower() == name.lower() and pk.cls_name(e['cls']) == 'StaticMesh':
            if group and group_of(pk, e)[-1:] == [group.split('.')[-1]]:
                return pk, e
            best = best or (pk, e)
    return best if best else (None, None)


class Builder:
    def __init__(self):
        self.tex_blobs = []
        self.tex_index = {}
        self.meshes = []
        self.mesh_index = {}
        self.vmeshes = []
        self.vmesh_index = {}
        self.effects = []

    # ---- textures
    def texture(self, pkgname, group, name, cls):
        key = (pkgname.lower(), name.lower())
        if key in self.tex_index:
            return self.tex_index[key]
        tp = texpkg(pkgname)
        if tp is None:
            print('   !! texture package %s not found (%s)' % (pkgname, name))
            self.tex_index[key] = -1
            return -1
        try:
            if cls == 'Texture':
                idx = fx_texture_index(dict(name=name, tp=tp), {}, self.tex_blobs)
            else:                                                 # shader / final blend / combiner: its diffuse picture
                mt = tp.material(group.split('.')[-1] if group else '', name)
                idx = self._rgba_texture(name, mt['diffuse'])
        except Exception as ex:
            print('   !! texture %s.%s: %s' % (pkgname, name, ex))
            idx = -1
        self.tex_index[key] = idx
        return idx

    def _rgba_texture(self, name, a):
        from PIL import Image
        im = Image.fromarray(a, 'RGBA')
        while max(im.size) > 512:
            im = im.resize((im.size[0] // 2, im.size[1] // 2), Image.LANCZOS)
        w, h = im.size
        levels = []
        while True:
            b = io.BytesIO()
            im.save(b, 'DDS', pixel_format='DXT5')
            levels.append(b.getvalue()[128:])
            if w <= 4 or h <= 4:
                break
            w //= 2
            h //= 2
            im = im.resize((w, h), Image.LANCZOS)
        w, h = Image.fromarray(a, 'RGBA').size
        while max(w, h) > 512:
            w //= 2
            h //= 2
        self.tex_blobs.append(cstr(name) + struct.pack('<IIII', 8, w, h, len(levels)) + b''.join(struct.pack('<I', len(l)) + l for l in levels))
        print('   shader texture %s %dx%d' % (name, w, h))
        return len(self.tex_blobs) - 1

    # ---- materials: base texture, a second (panning) texture multiplied over it, blend mode of the shader
    BLEND = {0: 1, 1: 1, 2: 2, 3: 3, 5: 6, 6: 5}                        # Shader.OutputBlending -> draw style (4 = invisible: no entry)

    def material_info(self, pkgname, group, name, cls):
        """-> (tex, tex2, panU, panV, drawStyle or None)"""
        tp = texpkg(pkgname)
        if tp is None:
            return (self.texture(pkgname, group, name, cls), -1, 0.0, 0.0, None)
        e = tp.find(group.split('.')[-1] if group else '', name)
        if e is None:
            for x in tp.pk.exports:
                if x['name'].lower() == name.lower():
                    e = x
                    break
        if e is None:
            return (self.texture(pkgname, group, name, cls), -1, 0.0, 0.0, None)
        blend = None
        walk = e
        for _ in range(4):
            c = tp.pk.cls_name(walk['cls'])
            if c == 'Shader':
                ob = FX._props(tp, walk).get('OutputBlending')
                if ob is not None:
                    blend = self.BLEND.get(ob)
                break
            if c in ('FinalBlend', 'Combiner'):
                n = FX._node(tp, walk, FX._props(tp, walk).get('Material' if c == 'FinalBlend' else 'Material1'))
                if not n:
                    break
                walk = n[1]
                continue
            break
        try:
            texs = FX.eval_textures(tp, e)
        except Exception:
            texs = []
        if not texs:
            return (self.texture(pkgname, group, name, cls), -1, 0.0, 0.0, blend)
        idx = []
        for tx in texs[:2]:
            try:
                idx.append((fx_texture_index(dict(name=tx['name'], tp=tx['tp']), {}, self.tex_blobs) if (tx['tp'], tx['name'].lower()) not in self.tex_index else self.tex_index[(tx['tp'], tx['name'].lower())], tx))
            except Exception as ex:
                print('   !! material texture %s: %s' % (tx['name'], ex))
                idx.append((-1, tx))
            self.tex_index[(tx['tp'], tx['name'].lower())] = idx[-1][0]
        tex = idx[0][0]
        tex2, pu, pv = -1, 0.0, 0.0
        if len(idx) > 1:
            tex2, pu, pv = idx[1][0], idx[1][1]['panU'], idx[1][1]['panV']
        return (tex, tex2, pu, pv, blend)

    # ---- vertex-animated meshes (wings)
    def vmesh(self, effect_pk, ref):
        r = ES.resolve(effect_pk, ref)
        if r is None or r[2].lower() != 'wing_high':
            return -1, None
        key = r[2].lower()
        if key in self.vmesh_index:
            return self.vmesh_index[key]
        vpk = l2pkg.Package(os.path.join(ROOT, 'Animations', r[0] + '.ukx'))
        e = VM.find(vpk, r[2])
        if e is None:
            return -1, None
        try:
            m = VM.decode(vpk, e)
        except Exception as ex:
            print('   !! vertex mesh %s: %s' % (r[2], ex))
            self.vmesh_index[key] = (-1, None)
            return -1, None
        uv = VM.planar_uv(m['frames'])
        blob = cstr(r[2]) + struct.pack('<III', m['nv'], m['nf'], len(m['tris']))
        for tri in m['tris']:
            blob += struct.pack('<3H', *tri)
        for i in range(m['nv']):
            blob += struct.pack('<2f', float(uv[i, 0]), float(uv[i, 1]))
        blob += m['frames'].astype('<f4').tobytes()
        self.vmeshes.append(blob)
        own = None
        try:                                                              # Materials = array of plain object references
            d = vpk.read_obj(e)
            rd = l2pkg.Reader(d)
            if vpk.names[rd.cidx()] == 'Materials':
                info = rd.u8(); sz = rd.u8() if ((info >> 4) & 7) == 5 else 4
                n = rd.cidx()
                refs = [rd.cidx() for _ in range(n)]
                own = ES.resolve(vpk, refs[0]) if refs else None
        except Exception:
            own = None
        self.vmesh_index[key] = (len(self.vmeshes) - 1, own)
        print('   vertex mesh %s: %d verts %d frames %d tris' % (r[2], m['nv'], m['nf'], len(m['tris'])))
        return self.vmesh_index[key]

    # ---- meshes
    def mesh(self, effect_pk, ref, custom_ref=None):
        r = ES.resolve(effect_pk, ref)
        if r is None:
            return -1, -1, None
        pkgname, group, name, _ = r
        key = (pkgname, group, name, custom_ref)
        if key in self.mesh_index:
            return self.mesh_index[key]
        pk, e = find_static(pkgname, group, name)
        if e is None:
            print('   !! static mesh %s.%s.%s not found' % (pkgname, group, name))
            self.mesh_index[key] = (-1, -1, None)
            return -1, -1, None
        try:
            m = ES.decode(pk, e)
        except Exception as ex:
            print('   !! static mesh %s: %s' % (name, ex))
            self.mesh_index[key] = (-1, -1, None)
            return -1, -1, None
        tex = -1
        mref = None
        if custom_ref:
            mref = ES.resolve(effect_pk, custom_ref)
        elif m['materials'] and m['materials'][0]:
            mref = m['materials'][0]
        if mref:
            tex = self.texture(mref[0], mref[1], mref[2], mref[3])
        blob = cstr('%s.%s.%s' % (pkgname, group, name)) + struct.pack('<IIi', len(m['verts']), len(m['tris']), tex)
        for (x, y, z), (u, v) in zip(m['verts'], m['uvs']):
            blob += struct.pack('<5f', x, y, z, u, v)
        for t in m['tris']:
            blob += struct.pack('<3H', *t)
        self.meshes.append(blob)
        self.mesh_index[key] = (len(self.meshes) - 1, tex, mref)
        print('   mesh %s.%s: %d verts %d tris texture %d' % (group, name, len(m['verts']), len(m['tris']), tex))
        return self.mesh_index[key]

    # ---- emitters
    def emitter(self, pk, e):
        kind = pk.cls_name(e['cls'])
        if kind not in ('SpriteEmitter', 'MeshEmitter', 'VertMeshEmitter'):
            return None
        P = U.parse(pk, pk.read_obj(e), 0)
        if P.get('Disabled'):
            return None
        mesh = tex = -1
        tex2, pu, pv, blend = -1, 0.0, 0.0, None
        if kind == 'MeshEmitter':
            if 'StaticMesh' not in P:
                return None
            custom = None
            cm = P.get('CustomMaterials')
            if cm:
                custom = cm[0]
            mesh, tex, mref = self.mesh(pk, P['StaticMesh'], custom)
            if mesh < 0:
                return None
            if mref:
                tex, tex2, pu, pv, blend = self.material_info(*mref)
        elif kind == 'VertMeshEmitter':
            if 'VertexMesh' not in P:
                return None
            mesh, own = self.vmesh(pk, P['VertexMesh'])
            if mesh < 0:
                return None
            cm = P.get('CustomMaterials')
            mref = ES.resolve(pk, cm[0]) if cm else own
            if not mref:
                return None
            tex, tex2, pu, pv, blend = self.material_info(*mref)
            if tex < 0:
                return None
        else:
            if 'Texture' not in P:
                return None
            r = ES.resolve(pk, P['Texture'])
            if r is None:
                return None
            tex = self.texture(r[0], r[1], r[2], r[3])
            if tex < 0:
                return None
        rv = lambda k, d=((0.0, 0.0),) * 3: U.rangevec(P.get(k), d)
        size_default = ((1.0, 1.0),) * 3 if kind != 'SpriteEmitter' else ((100.0, 100.0),) * 3
        life = U.rng(P.get('LifetimeRange'), (4.0, 4.0))
        delay = U.rng(P.get('InitialDelayRange'), (0.0, 0.0))
        loc = rv('StartLocationRange'); vel = rv('StartVelocityRange'); vloss = rv('VelocityLossRange')
        size = rv('StartSizeRange', size_default); sstart = rv('StartSpinRange'); srate = rv('SpinsPerSecondRange')
        cmul = rv('ColorMultiplierRange', ((1.0, 1.0),) * 3)
        acc = P.get('Acceleration', (0.0, 0.0, 0.0))
        flags = (1 if P.get('RenderTwoSided') else 0) | (2 if P.get('UseColorScale') else 0) | (4 if P.get('FadeIn') else 0) | (8 if P.get('FadeOut') else 0) \
            | (16 if (P.get('UseSizeScale') or P.get('UseRegularSizeScale')) else 0) | (32 if P.get('SpinParticles') else 0) | (64 if P.get('UniformSize') else 0) \
            | (128 if P.get('RespawnDeadParticles', True) else 0)
        flags2 = (1 if P.get('AutomaticInitialSpawning', True) else 0) | (2 if P.get('UseParticleColor') else 0) | (4 if P.get('CoordinateSystem', 1) == 1 else 0) \
            | (8 if P.get('BlendBetweenSubdivisions') else 0) | (16 if P.get('UseRandomSubdivision') else 0)
        draw = P.get('DrawStyle', 3)
        if kind != 'SpriteEmitter' and P.get('UseMeshBlendMode', True) and blend is not None:
            draw = blend                                                      # the material's own blend mode (shader OutputBlending)
        b = struct.pack('<4B', {'SpriteEmitter': 0, 'MeshEmitter': 1, 'VertMeshEmitter': 2}[kind], draw, flags, flags2)
        b += struct.pack('<ii', tex, mesh)
        b += struct.pack('<iff', tex2, pu, pv)
        b += struct.pack('<f i', P.get('Opacity', 1.0), P.get('MaxParticles', 10))
        b += struct.pack('<2f 2f 2f 2f', life[0], life[1], P.get('InitialParticlesPerSecond', 0.0), P.get('ParticlesPerSecond', 0.0), delay[0], delay[1], P.get('FadeInEndTime', 0.0), P.get('FadeOutStartTime', 0.0))
        for rr in (loc, vel, vloss):
            b += struct.pack('<6f', *[x for pair in rr for x in pair])
        b += struct.pack('<3f', *acc)
        for rr in (size, sstart, srate, cmul):
            b += struct.pack('<6f', *[x for pair in rr for x in pair])
        cs = P.get('ColorScale') or []
        b += struct.pack('<f I', P.get('ColorScaleRepeats', 1.0) or 1.0, len(cs))
        for c in cs:
            col = c.get('Color', (255, 255, 255, 255))
            b += struct.pack('<f4B', c.get('RelativeTime', 0.0), col[0], col[1], col[2], col[3])
        # location shape / velocity direction / revolution / auto reset / warm-up (EFX2 extras)
        polar = rv('StartLocationPolarRange'); srad = U.rng(P.get('SphereRadiusRange'), (0.0, 0.0)); lofs = P.get('StartLocationOffset', (0.0, 0.0, 0.0))
        rps = rv('RevolutionsPerSecondRange'); rcen = rv('RevolutionCenterOffsetRange'); ccw = P.get('SpinCCWorCW', (0.5, 0.5, 0.5))
        rsc = P.get('RevolutionScale') or []
        rflags = (1 if P.get('UseRevolution') else 0) | (2 if P.get('UseRevolutionScale') else 0) | (4 if P.get('AutoReset') else 0)
        arr = U.rng(P.get('AutoResetTimeRange'), (0.0, 0.0))
        extra = struct.pack('<4B', P.get('StartLocationShape', 0), P.get('GetVelocityDirectionFrom', 0), P.get('UseDirectionAs', 0), rflags)
        extra += struct.pack('<3f', *P.get('ProjectionNormal', (1.0, 0.0, 0.0)))
        extra += struct.pack('<6f 2f 3f 3f', *[x for pair in polar for x in pair], srad[0], srad[1], *lofs, *ccw)
        extra += struct.pack('<6f 6f 2f f', *[x for pair in rps for x in pair], *[x for pair in rcen for x in pair], arr[0], arr[1], P.get('RelativeWarmupTime', 0.0) or 0.0)
        extra += struct.pack('<I', len(rsc))
        for r_ in rsc:
            v = r_.get('RelativeRevolution', (0.0, 0.0, 0.0))
            extra += struct.pack('<4f', r_.get('RelativeTime', 0.0), v[0], v[1], v[2])
        ss = P.get('SizeScale') or []
        b += struct.pack('<4i I', P.get('TextureUSubdivisions', 1) or 1, P.get('TextureVSubdivisions', 1) or 1, P.get('SubdivisionStart', 0), P.get('SubdivisionEnd', 0), len(ss))
        for s in ss:
            b += struct.pack('<2f', s.get('RelativeTime', 0.0), s.get('RelativeSize', 1.0))
        return b + extra

    def effect(self, name):
        pk, ci = find_class(name)
        ems = []
        for i, e in enumerate(pk.exports):
            if e['pkg'] == ci and 'Emitter' in pk.cls_name(e['cls']):
                b = self.emitter(pk, e)
                if b is not None:
                    ems.append(b)
        print('effect %-26s %d emitters kept' % (name, len(ems)))
        self.effects.append(cstr(name) + struct.pack('<I', len(ems)) + b''.join(ems))

    def write(self, path):
        with open(path, 'wb') as f:
            f.write(b'EFX3' + struct.pack('<IIII', len(self.tex_blobs), len(self.meshes), len(self.vmeshes), len(self.effects)))
            for b in self.tex_blobs:
                f.write(b)
            for b in self.meshes:
                f.write(b)
            for b in self.vmeshes:
                f.write(b)
            for b in self.effects:
                f.write(b)
        print('wrote %s %.1f MB | textures %d meshes %d effects %d' % (path, os.path.getsize(path) / 1e6, len(self.tex_blobs), len(self.meshes), len(self.effects)))


if __name__ == '__main__':
    out = sys.argv[1]
    names = sys.argv[2:] or DEFAULT
    b = Builder()
    for n in names:
        b.effect(n)
    b.write(out)
