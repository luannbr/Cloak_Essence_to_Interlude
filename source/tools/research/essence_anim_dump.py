"""Explore the binary layout of an Essence *_cape_anim MeshAnimation object.
   python essence_anim_dump.py <LineageNewMantles.ukx> <AnimName> [--hex N]"""
import sys, os, re, struct
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader

pk = Package(sys.argv[1])
e = next(x for x in pk.exports if x['name'] == sys.argv[2])
d = pk.read_obj(e)
names = pk.names
print('object', e['name'], 'size', len(d), 'names in package', len(names))
n = int(sys.argv[sys.argv.index('--hex') + 1]) if '--hex' in sys.argv else 160
print('first %d bytes:' % n)
for i in range(0, n, 16):
    print('  %04x  %s' % (i, ' '.join('%02x' % b for b in d[i:i + 16])))

# where are Cape_* bone names referenced? scan for compact indexes whose name matches Cape_ and see the surrounding record layout
capeidx = {i for i, nm in enumerate(names) if re.match(r'(?i)cape_|mantle_pin', nm)}
r = Reader(d)
hits = []
for p in range(0, min(len(d), 20000)):
    r.p = p
    try:
        ni = r.cidx()
    except Exception:
        continue
    if ni in capeidx:
        hits.append((p, names[ni]))
print('cape name references in the first 20000 bytes: %d' % len(hits))
print(hits[:40])
