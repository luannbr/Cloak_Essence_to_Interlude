#!/usr/bin/env python3
"""Extract the visual EFFECT layers of an Essence material (env-map shine, animated fire/glow, self-illumination) as a small program.

A material is  FinalBlend -> Shader{Diffuse, Opacity, Specular x SpecularityMask, SelfIllumination x SelfIlluminationMask}  where Diffuse may be a Combiner
(op 5 = alpha blend of Material2 over Material1 through Mask).  The base texture (the `_ori`, colour + opacity) is drawn as before; every effect becomes a LAYER:

    layer = dict(kind='add'|'lerp', texs=[dict(name, pkg (utx path), env, panU, panV, mask)])

  add  : colour = product of the layer textures (the last one is the mask), added to the frame buffer (shine, glow)
  lerp : colour = product of the non-mask textures, alpha = alpha of the mask texture, drawn over the lit base (Combiner op 5: lava / fire in the feathers)
  env  : the texture is a sphere/reflection map (camera-space normal coordinates)
  pan  : texture coordinates scroll by (panU, panV) per second (TexPanner)

  extract(tex_package_for_utx, group, name) -> (base_texture_name, layers)
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import struct
from essence_tex import read_props
from essence_dxt import TexPackage

ROOT = os.environ.get('L2_ESSENCE_ROOT', '')
_PKGS = {}


def _utx_path(pkgname):
    d = os.path.join(ROOT, 'SysTextures')
    for f in os.listdir(d):
        if f.lower() == pkgname.lower() + '.utx':
            return os.path.join(d, f)
    return None


def _pkg(path):
    if path not in _PKGS:
        _PKGS[path] = TexPackage(path)
    return _PKGS[path]


def _import_ref(tp, ref):
    """'imp:name' -> (TexPackage, export) of the imported texture found through the import table's outer package; None when not found"""
    name = ref[4:]
    pk = tp.pk
    for im in pk.imports:
        if im[3] != name:
            continue
        outer = im[2]
        chain = []
        while outer < 0 and len(chain) < 4:
            o = pk.imports[-outer - 1]; chain.append(o[3]); outer = o[2]
        for pn in chain:
            p = _utx_path(pn)
            if p:
                t2 = _pkg(p)
                for e in t2.pk.exports:
                    if e['name'].lower() == name.lower() and t2.pk.cls_name(e['cls']) in ('Texture', 'TexPanner', 'Combiner', 'TexEnvMap', 'Shader', 'FinalBlend'):
                        return t2, e
    return None


def _node(tp, owner, ref):
    """resolve a property reference to (TexPackage, export entry) or None"""
    if not isinstance(ref, str):
        return None
    if ref.startswith('exp:'):
        e = tp.find_in_group_of(owner, ref[4:])
        return (tp, e) if e is not None else None
    if ref.startswith('imp:'):
        return _import_ref(tp, ref)
    return None


def _props(tp, e):
    try:
        return read_props(tp.pk, tp.pk.read_obj(e))[0]
    except Exception:
        return {}


def _pan(props):
    raw = props.get('PanDirection')
    rate = props.get('PanRate', 0.1)
    if isinstance(raw, str) and len(raw) >= 24:
        try:
            pitch, yaw, roll = struct.unpack('<3i', bytes.fromhex(raw[:24]))
        except Exception:
            return 0.0, 0.0
        a = yaw * 2.0 * math.pi / 65536.0
        return math.cos(a) * rate, math.sin(a) * rate
    return 0.0, rate


def eval_textures(tp, e, depth=0):
    """texture list [dict(tp, name, env, panU, panV)] describing the colour of a material node (a multiply chain)"""
    cls = tp.pk.cls_name(e['cls'])
    if depth > 5:
        return []
    if cls == 'Texture':
        return [dict(tp=tp, name=e['name'], env=False, panU=0.0, panV=0.0)]
    p = _props(tp, e)
    if cls == 'TexEnvMap':
        n = _node(tp, e, p.get('Material'))
        out = eval_textures(*n, depth + 1) if n else []
        for t in out: t['env'] = True
        return out
    if cls == 'TexPanner':
        n = _node(tp, e, p.get('Material'))
        out = eval_textures(*n, depth + 1) if n else []
        u, v = _pan(p)
        for t in out:
            t['panU'] += u; t['panV'] += v
        return out
    if cls in ('TexRotator', 'TexScaler', 'TexOscillator', 'ColorModifier'):
        n = _node(tp, e, p.get('Material'))
        return eval_textures(*n, depth + 1) if n else []
    if cls == 'Combiner':
        n1 = _node(tp, e, p.get('Material1')); n2 = _node(tp, e, p.get('Material2'))
        a = eval_textures(*n1, depth + 1) if n1 else []
        b = eval_textures(*n2, depth + 1) if n2 else []
        op = p.get('CombineOperation', 0)
        if op == 2:                                   # multiply
            return a + b
        if op == 1:
            return b
        return a + b if op in (3, 6) else a
    if cls in ('Shader', 'FinalBlend'):
        n = _node(tp, e, p.get('Diffuse') or p.get('Material'))
        return eval_textures(*n, depth + 1) if n else []
    return []


def extract(tp, group, name):
    """-> (base texture name, layers) for the material `group.name` of the TexPackage `tp`"""
    e = tp.find(group, name)
    if e is None:
        return None, []
    cls = tp.pk.cls_name(e['cls'])
    p = _props(tp, e)
    if cls == 'FinalBlend':
        n = _node(tp, e, p.get('Material'))
        if not n:
            return None, []
        tp, e = n; cls = tp.pk.cls_name(e['cls']); p = _props(tp, e)
    layers = []
    base = None
    if cls == 'Texture':
        return e['name'], []
    if cls != 'Shader':
        return None, []
    # diffuse: a texture, or a combiner whose Material1 is the base
    d = _node(tp, e, p.get('Diffuse'))
    while d:
        dtp, de = d
        dcls = dtp.pk.cls_name(de['cls'])
        if dcls == 'Texture':
            base = de['name']; break
        if dcls == 'Combiner':
            dp = _props(dtp, de)
            n1 = _node(dtp, de, dp.get('Material1')); n2 = _node(dtp, de, dp.get('Material2')); nm = _node(dtp, de, dp.get('Mask'))
            op = dp.get('CombineOperation', 0)
            if n2 and nm and op in (5, 7):
                fx = eval_textures(*n2)
                mk = eval_textures(*nm)
                if fx and mk:
                    layers.append(dict(kind='lerp' if op == 5 else 'add', texs=[dict(t, mask=False) for t in fx[:2]] + [dict(mk[-1], mask=True)]))
            d = n1
            continue
        break
    # specular (env map shine / animated) x mask, self illumination x mask
    for key, mkey in (('Specular', 'SpecularityMask'), ('SelfIllumination', 'SelfIlluminationMask')):
        s = _node(tp, e, p.get(key)); m = _node(tp, e, p.get(mkey))
        if s and m:
            fx = eval_textures(*s); mk = eval_textures(*m)
            if fx and mk:
                layers.append(dict(kind='add', texs=[dict(t, mask=False) for t in fx[:2]] + [dict(mk[-1], mask=True)]))
    if base is None:
        # Shader whose diffuse is not a plain texture chain (Aegis uses the _sp as diffuse): fall back to the first texture found
        t = eval_textures(tp, e)
        base = t[0]['name'] if t else None
    return base, layers


def describe(layers):
    out = []
    for l in layers:
        out.append('%s[%s]' % (l['kind'], ' x '.join('%s%s%s%s' % (t['name'], ' env' if t['env'] else '', ' pan(%.2f,%.2f)' % (t['panU'], t['panV']) if t['panU'] or t['panV'] else '', ' MASK' if t.get('mask') else '') for t in l['texs'])))
    return ' | '.join(out)


if __name__ == '__main__':
    tp = TexPackage(sys.argv[1])
    b, ls = extract(tp, sys.argv[2], sys.argv[3])
    print(b, describe(ls))
