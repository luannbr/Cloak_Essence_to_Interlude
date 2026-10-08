"""From the end of the Points array, walk the following arrays: try (count, stride) pairs and chain them.
   python essence_mesh_chain.py <pkg.ukx> <MeshName> <points_end_offset>"""
import sys, os, struct, math
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader

pk = Package(sys.argv[1])
e = next(x for x in pk.exports if x['name'] == sys.argv[2])
d = pk.read_obj(e)
start = int(sys.argv[3])
print('size', len(d), 'start', start)
for i in range(0, 96, 16):
    print('  %06x  %s' % (start + i, ' '.join('%02x' % b for b in d[start + i:start + i + 16])))

def walk(p, depth=0, path=()):
    if depth >= 6:
        yield path; return
    r = Reader(d); r.p = p
    try:
        n = r.cidx()
    except Exception:
        return
    if not 0 <= n <= 400000:
        return
    q = r.p
    tried = False
    for stride in (2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 44, 48):
        end = q + n * stride
        if end > len(d) - 1:
            continue
        r2 = Reader(d); r2.p = end
        try:
            n2 = r2.cidx()
        except Exception:
            continue
        if 0 <= n2 <= 400000 and n > 0:
            tried = True
            yield from walk(end, depth + 1, path + ((p, n, stride),))
    if not tried and path:
        yield path

# beam: prefer chains whose element strides look like known structures; print the first several chains
seen = 0
for chain in walk(start):
    if len(chain) >= 3:
        print('chain:', ' -> '.join('@%d n=%d x%d' % c for c in chain))
        seen += 1
        if seen >= 25: break
