"""Inspect an Essence mantle SkeletalMesh and its cape MeshAnimation (Ver111 packages).
   python essence_inspect.py <LineageNewMantles.ukx> <MeshName> <AnimName>"""
import sys, os, re, struct, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from l2pkg import Package, Reader

BONE = re.compile(r'bip|bone|dummy|pelvis|cape|mantle|nub|hair|tail|skirt', re.I)


def parse_bones(pkg, data, p, with_color, lo=3):
    r = Reader(data); r.p = p
    try:
        count = r.cidx()
        if not (lo <= count <= 400):
            return None
        out = []
        for i in range(count):
            ni = r.cidx()
            if not (0 < ni < len(pkg.names)):
                return None
            nm = pkg.names[ni]
            if not BONE.search(nm):
                return None
            flags = r.u32()
            fl = [r.f32() for _ in range(11)]
            if any(f != f or abs(f) > 1e6 for f in fl):
                return None
            nch = r.i32(); par = r.i32()
            if with_color:
                r.u32()
            if not (0 <= nch <= 128) or not (-1 <= par < max(i, 1) + 0):
                return None
            out.append((nm, par, nch))
        return out, r.p
    except Exception:
        return None


def find_bones(pkg, data, limit=20000, lo=3):
    best = None
    for p in range(1, min(len(data), limit)):
        for wc in (False, True):
            res = parse_bones(pkg, data, p, wc, lo)
            if res and (best is None or len(res[0]) > len(best[0][0])):
                best = (res, p, wc)
        if best and best[0][0] and len(best[0][0]) >= 20:
            break
    return best


def main():
    pk = Package(sys.argv[1])
    mesh = next(e for e in pk.exports if e['name'] == sys.argv[2])
    data = pk.read_obj(mesh)
    print('mesh', mesh['name'], 'class', pk.cls_name(mesh['cls']), 'size', len(data))
    b = find_bones(pk, data)
    if not b:
        print('  bones not found'); return
    (bones, endp), p0, wc = b
    print('  RefSkeleton: %d bones at %d (color=%s), ends %d' % (len(bones), p0, wc, endp))
    print('  ', [x[0] for x in bones])
    # weights
    nb = len(bones)
    best = None
    for p in range(40, len(data) - 8):
        r = Reader(data); r.p = p
        try:
            n = r.cidx()
        except Exception:
            continue
        if not 50 <= n <= 100000 or r.p + 8 * n > len(data):
            continue
        ok = 0; bypt = collections.defaultdict(float)
        for i in range(min(n, 400)):
            w, pt, bi = struct.unpack_from('<fHH', data, r.p + 8 * i)
            if 0.0005 <= w <= 1.0001 and bi < nb:
                ok += 1; bypt[pt] += w
        if ok < min(n, 400) * 0.98:
            continue
        groups = [s for s in bypt.values() if s > 0]
        good = sum(1 for s in groups[:-1] if 0.98 <= s <= 1.02)
        if len(groups) < 10 or good < 0.9 * (len(groups) - 1):
            continue
        tot_ok = sum(1 for i in range(n) if (lambda t: 0.0005 <= t[0] <= 1.0001 and t[2] < nb)(struct.unpack_from('<fHH', data, r.p + 8 * i)))
        if tot_ok >= n * 0.97 and (best is None or n > best[1]):
            best = (p, n, r.p)
    if best:
        p, n, start = best
        wsum = collections.Counter(); cnt = collections.Counter(); pts = set(); per = collections.Counter()
        for i in range(n):
            w, pt, bi = struct.unpack_from('<fHH', data, start + 8 * i)
            wsum[bi] += w; cnt[bi] += 1; pts.add(pt); per[pt] += 1
        tot = sum(wsum.values())
        print('  influences: %d records, %d vertices, max %d influences/vertex, bones with weight: %d' % (n, len(pts), max(per.values()), len(wsum)))
        for bi, w in wsum.most_common(14):
            print('    bone %3d %-18s weight %7.1f (%4.1f%%) influences %d' % (bi, bones[bi][0], w, 100 * w / tot, cnt[bi]))
    else:
        print('  influences not found')

    anim = next(e for e in pk.exports if e['name'] == sys.argv[3])
    ad = pk.read_obj(anim)
    print('anim', anim['name'], 'class', pk.cls_name(anim['cls']), 'size', len(ad))
    ab = find_bones(pk, ad, limit=60000, lo=3)
    if ab:
        (abones, aend), ap0, awc = ab
        print('  RefBones: %d at %d (color=%s) ends %d' % (len(abones), ap0, awc, aend))
        print('  ', [x[0] for x in abones][:90])
    # names that look like sequence names
    seqlike = [n for n in pk.names if re.match(r'^(run|walk|idle|wait|sit|stand|atk|attack|jump|death|dead|swim|fly|social|spell|cast|hit|damage|stun|fall|ride|dash|move|combat)', n, re.I)]
    print('  sequence-like names in package (%d): %s' % (len(seqlike), seqlike[:70]))


main()
