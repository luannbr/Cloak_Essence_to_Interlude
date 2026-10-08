"""Find the Points / Wedges / Faces arrays of an Ver111 SkeletalMesh by plausibility.
   python essence_mesh_scan.py <pkg.ukx> <MeshName>"""
import sys, os, struct, math
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader

pk = Package(sys.argv[1])
e = next(x for x in pk.exports if x['name'] == sys.argv[2])
d = pk.read_obj(e)
print('mesh', e['name'], 'size', len(d))
NPOINT = int(sys.argv[3]) if len(sys.argv) > 3 else None

def cidx_at(p):
    r = Reader(d); r.p = p
    try:
        n = r.cidx()
    except Exception:
        return None, p
    return n, r.p

# --- float3 arrays (points / normals): count n, then n*12 bytes of finite floats in a sane range
cands = []
for p in range(0, len(d) - 16):
    n, q = cidx_at(p)
    if n is None or n < 30 or n > 200000 or q + 12 * n > len(d):
        continue
    ok = True; mn = [1e30] * 3; mx = [-1e30] * 3
    for i in range(0, n, max(1, n // 60)):
        x, y, z = struct.unpack_from('<3f', d, q + 12 * i)
        if not all(math.isfinite(v) and abs(v) < 2000 for v in (x, y, z)):
            ok = False; break
        for k, v in enumerate((x, y, z)):
            mn[k] = min(mn[k], v); mx[k] = max(mx[k], v)
    if ok and max(mx[k] - mn[k] for k in range(3)) > 5:
        cands.append((p, n, q, mn, mx))
print('float3 arrays (n, at, end, extent):')
seen = set()
for p, n, q, mn, mx in cands:
    if (n, q) in seen: continue
    seen.add((n, q))
    print('  at %7d  n=%6d  data %7d..%7d  min(%.1f %.1f %.1f) max(%.1f %.1f %.1f)' % (p, n, q, q + 12 * n, *mn, *mx))
    if len(seen) > 40: break

# --- face-like arrays: records of 3 or 4 WORDs referencing indices < some bound
print('index-triple arrays (records of 6/8/10/12 bytes, all indices increasing sanely):')
found = 0
for stride in (6, 8, 10, 12, 16):
    for p in range(0, len(d) - 16):
        n, q = cidx_at(p)
        if n is None or n < 100 or n > 200000 or q + stride * n > len(d):
            continue
        mxi = 0; ok = True
        for i in range(0, n, max(1, n // 80)):
            w = struct.unpack_from('<3H', d, q + stride * i)
            if len(set(w)) < 3: ok = False; break
            mxi = max(mxi, *w)
        if ok and mxi < 70000 and mxi >= 0.5 * n * 0.5:
            print('  stride %2d at %7d n=%6d data %7d..%7d max index %d' % (stride, p, n, q, q + stride * n, mxi))
            found += 1
            if found > 25: break
    if found > 25: break
