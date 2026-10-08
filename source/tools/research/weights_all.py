"""Bone-weight census of every cloak mesh in a ukx: which bones carry weight, per cloak id.
   python weights_all.py <LineShieldCloaks.ukx> [out.txt]
Uses the same influence-array detection as mesh_weights.py (FVertInfluence = {float w; WORD point; WORD bone})."""
import sys, os, struct, collections, re
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader
from bones import refskeleton


def find_influences(data, nb):
    best = None
    for p in range(40, len(data) - 8):
        r = Reader(data); r.p = p
        try:
            n = r.cidx()
        except Exception:
            continue
        if not 50 <= n <= 60000 or r.p + 8 * n > len(data):
            continue
        ok = 0; bypt = collections.defaultdict(float)
        for i in range(min(n, 400)):
            w, pt, b = struct.unpack_from('<fHH', data, r.p + 8 * i)
            if 0.001 <= w <= 1.0001 and b < nb:
                ok += 1; bypt[pt] += w
        if ok < min(n, 400) * 0.98:
            continue
        groups = [s for s in bypt.values() if s > 0]
        good = sum(1 for s in groups[:-1] if 0.98 <= s <= 1.02)
        if len(groups) < 10 or good < 0.9 * (len(groups) - 1):
            continue
        tot_ok = sum(1 for i in range(n) if (lambda t: 0.001 <= t[0] <= 1.0001 and t[2] < nb)(struct.unpack_from('<fHH', data, r.p + 8 * i)))
        if tot_ok >= n * 0.97 and (best is None or n > best[1]):
            best = (p, n, r.p)
    return best


def main():
    pk = Package(sys.argv[1])
    out = open(sys.argv[2], 'w', encoding='utf-8') if len(sys.argv) > 2 else sys.stdout
    skel_cache = {}
    rows = []
    cloaks = [e for e in pk.exports if re.search(r'_Cloak_\d+$', e['name'])]
    print('%d cloak meshes' % len(cloaks), file=out); out.flush()
    for k, e in enumerate(cloaks):
        data = pk.read_obj(e)
        body = e['name'].split('_Cloak_')[0]
        sk = refskeleton(pk, e)
        if not sk:
            sk = skel_cache.get(body)
            if not sk:
                alt = next((x for x in cloaks if x['name'].startswith(body + '_Cloak_') and refskeleton(pk, x)), None)
                sk = refskeleton(pk, alt) if alt else None
        else:
            skel_cache[body] = sk
        if not sk:
            print('%-28s no skeleton' % e['name'], file=out); continue
        bn = [b[0] for b in sk]
        best = find_influences(data, len(bn))
        if not best:
            print('%-28s influences not found' % e['name'], file=out); continue
        p, n, start = best
        wsum = collections.Counter(); pts = set()
        for i in range(n):
            w, pt, b = struct.unpack_from('<fHH', data, start + 8 * i)
            wsum[b] += w; pts.add(pt)
        tot = sum(wsum.values())
        top = [(bn[b], round(100 * w / tot, 1)) for b, w in wsum.most_common(5)]
        multi = sum(1 for b, w in wsum.items() if w / tot > 0.01)
        rows.append((e['name'], len(bn), len(pts), n, multi, top))
        print('%-28s bones=%d verts=%d infl=%d bones>1%%=%d  %s' % (e['name'], len(bn), len(pts), n, multi, top), file=out); out.flush()
    print('\n== summary ==', file=out)
    dom = collections.Counter(r[5][0][0] for r in rows)
    print('dominant bone histogram:', dict(dom), file=out)
    print('meshes with >1 bone above 1%%: %d / %d' % (sum(1 for r in rows if r[4] > 1), len(rows)), file=out)
    for r in rows:
        if r[4] > 1:
            print('  MULTI', r[0], r[5], file=out)


main()
