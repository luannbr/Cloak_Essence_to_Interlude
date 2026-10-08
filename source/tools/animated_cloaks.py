"""List which cloak ids use animated materials (TexPanner / frame-animated textures / Combiner masks / env maps).
   python animated_cloaks.py <LineShieldCloaks.ukx> [Body=MFighter]"""
import sys, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
from ukx_materials import Pkg, cloak_materials

spec = importlib.util.spec_from_file_location('mt', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'material_tree.py'))
# reuse props() from material_tree without running its main()
src = open(spec.origin, encoding='utf-8').read().replace('\nmain()\n', '\n')
ns = {'__name__': 'mt', '__file__': spec.origin}
exec(compile(src, spec.origin, 'exec'), ns)
props = ns['props']

ukx = sys.argv[1]; body = sys.argv[2] if len(sys.argv) > 2 else 'MFighter'
p = Pkg(ukx); byname = {e[1]: e for e in p.exports}
mats, _ = cloak_materials(ukx)
cache = {}


def feats(name, seen=()):
    if name in cache: return cache[name]
    e = byname.get(name); out = set()
    if not e or name in seen: return out
    cls = p.cls(e[0])
    pr = props(p, p.obj(e)[:600] if cls == 'Texture' else p.obj(e))
    if cls == 'Texture' and 'AnimNext' in pr: out.add('frames')
    if cls == 'TexPanner': out.add('panner')
    if cls == 'Combiner': out.add('combiner')
    if cls == 'TexEnvMap': out.add('envmap')
    for k, v in pr.items():
        if isinstance(v, tuple) and v[0] == 'obj' and v[1]:
            out |= feats(v[1], seen + (name,))
    cache[name] = out
    return out


ids = sorted({i for (b, i) in mats if b == body})
rows = []
for i in ids:
    f = set()
    for _, n in mats[(body, i)]: f |= feats(n)
    rows.append((i, len(mats[(body, i)]), sorted(f)))
print('body', body, '| cloak id | #materials | features')
for i, n, f in rows:
    print('  %d  %d  %s' % (i, n, ','.join(f) or '-'))
anim = [i for i, n, f in rows if {'panner', 'frames', 'combiner'} & set(f)]
print('\nids with animated material effects (panner/frames/combiner):', anim)
print('ids with only static materials (+ env map):', [i for i, n, f in rows if i not in anim])
