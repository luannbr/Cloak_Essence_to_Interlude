"""Print the material graph reachable from the Materials[] of a cloak mesh and flag animated parts.

  python material_tree.py <LineShieldCloaks.ukx> <Body> <id> [<Body> <id> ...]
"""
import sys, os, struct
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ukx_materials import Pkg, cloak_materials

SIZES = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}


def props(p, data):
    c = Pkg.Cur(data); out = {}
    while True:
        nm = p.names[c.idx()]
        if nm == 'None': break
        info = c.u8(); t = info & 0xF; sz = (info >> 4) & 7; arr = bool(info & 0x80)
        if t == 10: c.idx()
        if sz in SIZES: size = SIZES[sz]
        elif sz == 5: size = c.u8()
        elif sz == 6: size = struct.unpack_from('<H', c.d, c.p)[0]; c.p += 2
        else: size = c.u32()
        if arr and t != 3:
            b = c.u8()
            if b >= 128:
                if b & 0xC0 == 0x80: c.u8()
                else: c.u8(); c.u8(); c.u8()
        raw = c.d[c.p:c.p + size] if t != 3 else b''
        if t != 3: c.p += size
        if t == 5:
            ci = Pkg.Cur(raw).idx()
            out[nm] = ('obj', p.exports[ci - 1][1] if 0 < ci <= len(p.exports) else None)
        elif t == 2: out[nm] = struct.unpack('<i', raw)[0]
        elif t == 4: out[nm] = round(struct.unpack('<f', raw)[0], 3)
        elif t == 1: out[nm] = raw[0] if len(raw) == 1 else raw.hex()
        elif t == 3: out[nm] = bool(info & 0x80)
        else: out[nm] = raw.hex()
    return out


def main():
    ukx = sys.argv[1]
    p = Pkg(ukx)
    byname = {e[1]: e for e in p.exports}
    mats, _ = cloak_materials(ukx)
    flags = set()

    def show(name, depth, seen):
        e = byname.get(name)
        if not e: print('  ' * depth + '%s ?' % name); return
        cls = p.cls(e[0]); pr = props(p, p.obj(e)) if e[2] < 3000 or cls != 'Texture' else props(p, p.obj(e)[:400])
        extra = ''
        if cls == 'Texture':
            extra = ' %sx%s%s' % (pr.get('USize'), pr.get('VSize'), ' AnimNext=%s' % pr['AnimNext'][1] if 'AnimNext' in pr else '')
            if 'AnimNext' in pr: flags.add('frame-animated texture')
        if cls == 'TexPanner': flags.add('TexPanner (scrolling)'); extra = ' PanRate=%s' % pr.get('PanRate')
        if cls == 'Combiner': flags.add('Combiner'); extra = ' op=%s mask=%s' % (pr.get('CombineOperation'), pr.get('Mask', (0, None))[1] if isinstance(pr.get('Mask'), tuple) else None)
        if cls == 'TexEnvMap': flags.add('TexEnvMap (reflection)')
        if cls == 'FinalBlend': extra = ' alphaTest=%s ref=%s blend=%s twoSided=%s' % (pr.get('AlphaTest'), pr.get('AlphaRef'), pr.get('FrameBufferBlending'), pr.get('TwoSided'))
        print('  ' * depth + '%s %s%s' % (cls, name, extra))
        if name in seen or depth > 7: return
        seen = seen | {name}
        for k, v in pr.items():
            if isinstance(v, tuple) and v[0] == 'obj' and v[1] and v[1] != name and k not in ('AnimNext',):
                print('  ' * depth + '  .%s ->' % k)
                show(v[1], depth + 2, seen)

    args = sys.argv[2:]
    for i in range(0, len(args), 2):
        body, cid = args[i], int(args[i + 1])
        print('=== %s_Cloak_%d  materials: %s' % (body, cid, [n for _, n in mats[(body, cid)]]))
        for n, (cls, name) in enumerate(mats[(body, cid)]):
            print('-- section %d' % n)
            show(name, 1, frozenset())
    print('\nanimated features found:', sorted(flags) or 'none')


main()
