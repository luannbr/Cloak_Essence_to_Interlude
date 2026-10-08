#!/usr/bin/env python3
"""Material graph of an Essence UE2 material (FinalBlend -> Shader -> Combiner / TexPanner / TexEnvMap / Texture ...).

  graph(tp, group, name) -> nested dict(cls, name, props..., children)   (tp = essence_dxt.TexPackage)
  describe(node) -> one-line signature, e.g. 'FinalBlend(a2,AT160,2S)>Shader[D=Combiner5(ori,Panner(fire_wave02),mask sp2); S=EnvMap(nv_gold00) x sp]'
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence_tex import read_props

CLASSES = ('FinalBlend', 'Shader', 'Combiner', 'TexPanner', 'TexRotator', 'TexScaler', 'TexOscillator', 'TexEnvMap', 'ColorModifier', 'ConstantColor', 'Texture', 'Cubemap', 'VertexColor', 'TexMatrix', 'ScriptedTexture', 'TexCoordSource')
REFKEYS = ('Material', 'Diffuse', 'Opacity', 'Specular', 'SpecularityMask', 'SelfIllumination', 'SelfIlluminationMask', 'Detail', 'Material1', 'Material2', 'Mask')


def _find(tp, owner, name):
    e = tp.find_in_group_of(owner, name)
    return e


def graph(tp, group, name, _seen=None, _depth=0, _entry=None):
    e = _entry or tp.find(group, name)
    if e is None:
        return None
    cls = tp.pk.cls_name(e['cls'])
    node = dict(cls=cls, name=e['name'])
    if cls == 'Texture':
        try:
            props, _ = read_props(tp.pk, tp.pk.read_obj(e))
            node.update(fmt=props.get('Format'), w=props.get('USize'), h=props.get('VSize'))
        except Exception:
            pass
        return node
    if _depth > 8:
        return node
    try:
        props, _ = read_props(tp.pk, tp.pk.read_obj(e))
    except Exception as ex:
        node['error'] = str(ex)[:40]
        return node
    node['props'] = {k: v for k, v in props.items() if k not in REFKEYS}
    for k in REFKEYS:
        v = props.get(k)
        if isinstance(v, str) and v.startswith('exp:'):
            c = _find(tp, e, v[4:])
            if c is not None:
                node.setdefault('children', {})[k] = graph(tp, None, None, _depth=_depth + 1, _entry=c)
            else:
                node.setdefault('children', {})[k] = dict(cls='?', name=v)
        elif isinstance(v, str) and v.startswith('imp:'):
            node.setdefault('children', {})[k] = dict(cls='import', name=v[4:])
    return node


def describe(n):
    if n is None:
        return '-'
    cls = n['cls']
    if cls == 'Texture':
        return n['name']
    ch = n.get('children', {})
    p = n.get('props', {})
    if cls == 'FinalBlend':
        return 'FinalBlend(blend%s%s%s)>%s' % (p.get('FrameBufferBlending'), ',AT%s' % p.get('AlphaRef') if p.get('AlphaTest') else '', ',2S' if p.get('TwoSided') else '', describe(ch.get('Material')))
    if cls == 'Shader':
        s = 'Shader[D=%s' % describe(ch.get('Diffuse'))
        if 'Opacity' in ch: s += '; O=%s' % describe(ch['Opacity'])
        if 'Specular' in ch: s += '; S=%s x %s' % (describe(ch['Specular']), describe(ch.get('SpecularityMask')))
        if 'SelfIllumination' in ch: s += '; SI=%s x %s' % (describe(ch['SelfIllumination']), describe(ch.get('SelfIlluminationMask')))
        return s + ']'
    if cls == 'Combiner':
        return 'Combiner%s(%s, %s, mask %s)' % (p.get('CombineOperation'), describe(ch.get('Material1')), describe(ch.get('Material2')), describe(ch.get('Mask')))
    if cls in ('TexPanner', 'TexRotator', 'TexScaler', 'TexOscillator'):
        return '%s(%s)' % (cls, describe(ch.get('Material')))
    if cls == 'TexEnvMap':
        return 'EnvMap(%s)' % describe(ch.get('Material'))
    return '%s(%s)' % (cls, ','.join(describe(v) for v in ch.values()))
