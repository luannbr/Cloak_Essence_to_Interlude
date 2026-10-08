"""Which bones influence a cloak mesh?  (UE2 FVertInfluence = {float Weight; WORD PointIndex; WORD BoneIndex}, 8 bytes)
   python mesh_weights.py <LineShieldCloaks.ukx> <MeshName> [...]"""
import sys, os, struct, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader
from bones import refskeleton

pk = Package(sys.argv[1])
for name in sys.argv[2:]:
    e = next(x for x in pk.exports if x['name'] == name)
    data = pk.read_obj(e)
    sk = refskeleton(pk, e)
    if not sk:                                   # fall back to the skeleton of another cloak of the same body
        body = name.split('_Cloak_')[0]
        alt = next((x for x in pk.exports if x['name'].startswith(body + '_Cloak_') and refskeleton(pk, x)), None)
        sk = refskeleton(pk, alt) if alt else None
    nb = len(sk); bn = [b[0] for b in sk]
    best = None
    for p in range(40, len(data) - 8):
        r = Reader(data); r.p = p
        try:
            n = r.cidx()
        except Exception:
            continue
        if not 50 <= n <= 60000 or r.p + 8 * n > len(data): continue
        ok = 0; bypt = collections.defaultdict(float); order = []
        for i in range(min(n, 400)):
            w, pt, b = struct.unpack_from('<fHH', data, r.p + 8 * i)
            if 0.001 <= w <= 1.0001 and b < nb:
                ok += 1; bypt[pt] += w
        if ok < min(n, 400) * 0.98: continue
        # per-vertex weights of complete groups should sum to ~1
        groups = [s for s in bypt.values() if s > 0]
        good = sum(1 for s in groups[:-1] if 0.98 <= s <= 1.02)
        if len(groups) < 10 or good < 0.9 * (len(groups) - 1): continue
        tot_ok = sum(1 for i in range(n) if (lambda t: 0.001 <= t[0] <= 1.0001 and t[2] < nb)(struct.unpack_from('<fHH', data, r.p + 8 * i)))
        if tot_ok >= n * 0.97 and (best is None or n > best[1]): best = (p, n, r.p)
    print('== %s: %d bones, size %d' % (name, nb, len(data)))
    if not best:
        print('   vertex influences not found'); continue
    p, n, start = best
    wsum = collections.Counter(); cnt = collections.Counter(); pts = set()
    for i in range(n):
        w, pt, b = struct.unpack_from('<fHH', data, start + 8 * i)
        wsum[b] += w; cnt[b] += 1; pts.add(pt)
    print('   influences array @%d: %d records, %d distinct vertices' % (p, n, len(pts)))
    tot = sum(wsum.values())
    for b, w in wsum.most_common(12):
        print('   bone %2d %-22s weight %7.1f (%4.1f%%)  influences %d' % (b, bn[b], w, 100 * w / tot, cnt[b]))
    capes = [b for b in range(nb) if 'cape' in bn[b].lower()]
    print('   cape bones:', [(b, bn[b], round(100 * wsum[b] / tot, 1)) for b in capes])
