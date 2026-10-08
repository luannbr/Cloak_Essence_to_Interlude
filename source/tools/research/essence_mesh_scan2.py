"""Locate index buffers / UV streams / influence arrays of an Essence SkeletalMesh given the vertex count.
   python essence_mesh_scan2.py <pkg.ukx> <MeshName> <nverts>"""
import sys, os, struct, math, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader

pk = Package(sys.argv[1])
e = next(x for x in pk.exports if x['name'] == sys.argv[2])
d = pk.read_obj(e)
NV = int(sys.argv[3])
print('size', len(d), 'nverts', NV)


def cidx_at(p):
    r = Reader(d); r.p = p
    try:
        n = r.cidx()
    except Exception:
        return None, p
    return n, r.p


# 1) u16 index arrays: n % 3 == 0, all < NV, using many distinct vertices
print('--- u16 index arrays (n%3==0, all values < nverts, many distinct):')
for p in range(0, len(d) - 8):
    n, q = cidx_at(p)
    if n is None or n < 150 or n % 3 or q + 2 * n > len(d):
        continue
    vals = struct.unpack_from('<%dH' % n, d, q)
    if max(vals) >= NV:
        continue
    distinct = len(set(vals))
    if distinct < NV * 0.6:
        continue
    degenerate = sum(1 for i in range(0, n, 3) if len({vals[i], vals[i + 1], vals[i + 2]}) < 3)
    print('  at %7d  n=%6d (%d tris) data %7d..%7d  distinct %d  degenerate %d' % (p, n, n // 3, q, q + 2 * n, distinct, degenerate))

# 2) float2 arrays with n == NV (UVs) and small stride variants
print('--- float2 arrays with n==nverts:')
for p in range(0, len(d) - 8):
    n, q = cidx_at(p)
    if n != NV:
        continue
    for stride in (8, 12, 16, 20, 24, 28, 32, 36, 40):
        if q + stride * n > len(d):
            continue
        ok = True
        for i in range(0, n, max(1, n // 50)):
            vals = struct.unpack_from('<%df' % (stride // 4), d, q + stride * i)
            if not all(math.isfinite(v) and abs(v) < 5000 for v in vals[:2]):
                ok = False; break
        if ok:
            u = [struct.unpack_from('<2f', d, q + stride * i) for i in range(0, n, max(1, n // 200))]
            print('  count@%d stride %2d data %d..%d  u %.2f..%.2f v %.2f..%.2f' % (p, stride, q, q + stride * n, min(a for a, b in u), max(a for a, b in u), min(b for a, b in u), max(b for a, b in u)))

# 3) arrays with n == NV of any small stride (to find per-vertex streams)
print('--- arrays whose count == nverts (any stride 1..48), showing where the next count-like value follows:')
for p in range(0, len(d) - 8):
    n, q = cidx_at(p)
    if n != NV:
        continue
    print('  count at %d, data from %d' % (p, q))
