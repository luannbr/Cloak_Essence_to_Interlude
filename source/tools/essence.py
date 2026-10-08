"""Decoder for the Essence (file version 133, mesh format v7) mantles: LineageNewMantles.ukx.

  parse_mesh(pkg, export)  -> dict(points[N][3], wedges[(pt,u,v)], faces[(w0,w1,w2)], infl[(w,pt,bone)], bones[(name,parent,quat,pos)], materials[import refs], bbox)
  parse_anim(pkg, export)  -> dict(bones[(name,parent)], seqs[dict(name,start,frames,rate,chunk)], chunks[list of tracks: dict(quat[frames][4], pos[3], times)])

Layout (verified on MFighter_NewMantle00_m_ad00 / MFighter_cape_anim):
  mesh:  byte0 = 'None'; FBox(24)+valid; FSphere(16); int 7 (format version) @42; int NumPoints @46; Materials @51 (cidx n, n object refs);
         ... RefSkeleton (cidx n; per bone: name cidx, flags u32, quat(4f) pos(3f) len,x,y,z (4f), nchildren i32, parent i32)
         Points  = cidx N + N x 16-byte records (x,y,z float + packed dword)
         Faces   = cidx F + F x 3 u16 (wedge indices)         Influences = cidx I + I x {f32 weight, u16 point, u16 bone}
         Wedges  = cidx W + W x {u16 point, f32 u, f32 v}
  anim:  byte0 'None'; int 1; RefBones = cidx n + n x {name cidx, flags u32, parent i32}; Moves = one chunk per sequence:
         32-byte header (TrackTime float @20), BoneIndices (cidx n + n x i32), AnimTracks (cidx n; per track: flags u32,
         KeyQuat cidx+16B each (x,y,z,w), KeyPos cidx+12B each, KeyTime cidx+4B each), then 3 bytes; then
         AnimSeqs = cidx n + n x {name cidx, groups cidx(+names), StartFrame i32, NumFrames i32, notifys cidx(+...), Rate f32}
"""
import os, re, struct, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2pkg import Package, Reader

STRICT_FACES = True                                   # face array = the offset where every triangle index is valid (False: the old tolerant search only)
BONE_RE = re.compile(r'cape|mantle|dummy|bip|bone', re.I)


def _cidx_at(d, p):
    r = Reader(d); r.p = p
    try:
        return r.cidx(), r.p
    except Exception:
        return None, p


def parse_refskeleton(pkg, d, lo=60, hi=40000):
    best = None
    for p in range(lo, min(hi, len(d) - 70)):
        r = Reader(d); r.p = p
        try:
            n = r.cidx()
            if not 2 <= n <= 200:
                continue
            bones = []
            ok = True
            for i in range(n):
                ni = r.cidx()
                if not (0 < ni < len(pkg.names)) or not BONE_RE.search(pkg.names[ni]):
                    ok = False; break
                r.u32()
                f = [r.f32() for _ in range(11)]
                if any(x != x or abs(x) > 1e5 for x in f):
                    ok = False; break
                nch = r.i32(); par = r.i32()
                if not (0 <= nch <= 64 and -1 <= par < max(i, 1)):
                    ok = False; break
                bones.append((pkg.names[ni], par, tuple(f[0:4]), tuple(f[4:7])))
            if ok and (best is None or len(bones) > len(best[0])):
                best = (bones, p, r.p)
                if len(bones) >= 5:
                    return best
        except Exception:
            continue
    return best


def parse_mesh(pkg, e, pstride=16):
    d = pkg.read_obj(e)
    out = {'name': e['name'], 'size': len(d)}
    if struct.unpack_from('<i', d, 42)[0] != 7:
        raise ValueError('mesh format version %d (expected 7)' % struct.unpack_from('<i', d, 42)[0])
    NP = struct.unpack_from('<i', d, 46)[0]
    out['npoints'] = NP
    r = Reader(d); r.p = 51
    nm = r.cidx()
    out['materials'] = [r.cidx() for _ in range(nm)]               # object references (negative = import)
    out['bbox'] = struct.unpack_from('<6f', d, 1)
    sk = parse_refskeleton(pkg, d)
    if not sk:
        raise ValueError('refskeleton not found')
    out['bones'], sk_start, sk_end = sk
    NB = len(out['bones'])
    # --- points: cidx NP + NP x 16 bytes, spatially coherent sequence
    pts = None
    for p in range(sk_end, len(d) - pstride * NP):
        n, q = _cidx_at(d, p)
        if n != NP:
            continue
        P = [struct.unpack_from('<3f', d, q + pstride * i) for i in range(0, NP, max(1, NP // 200))]
        if not all(all(math.isfinite(c) and abs(c) < 3000 for c in v) for v in P):
            continue
        cons = sorted(math.dist(P[i], P[i + 1]) for i in range(len(P) - 1))
        ext = max(max(v[k] for v in P) - min(v[k] for v in P) for k in range(3))
        if ext > 3 and cons[len(cons) // 2] < ext * 0.25:
            pts = (p, q); break
    if not pts:
        raise ValueError('points not found')
    pstart, pdata = pts
    out['points'] = [struct.unpack_from('<3f', d, pdata + pstride * i) for i in range(NP)]
    pend = pdata + pstride * NP
    # --- wedge count: plain int right after the points; the wedge array itself is  cidx W + W x {u16 point, f32 u, f32 v}
    NW = struct.unpack_from('<i', d, pend)[0]
    wed = None
    cand = [NW] if NP <= NW <= 4 * NP else []
    for p in range(pend, len(d) - 10 * NP):
        n, q = _cidx_at(d, p)
        if n is None or q + 10 * n > len(d):
            continue
        if cand:
            if n != cand[0]:
                continue
        elif not NP <= n <= 3 * NP:
            continue
        ok = True
        for i in list(range(0, min(n, 8))) + list(range(0, n, max(1, n // 60))) + [n - 1]:
            pt, u, v = struct.unpack_from('<Hff', d, q + 10 * i)
            if pt >= NP or not (-2 < u < 4 and -2 < v < 4):
                ok = False; break
        if ok:
            wed = (p, q, n); break
    if not wed:
        raise ValueError('wedges not found')
    out['wedges'] = [struct.unpack_from('<Hff', d, wed[1] + 10 * i) for i in range(wed[2])]
    NW = wed[2]
    W = out['wedges']

    def uvd(a, b):
        return math.hypot(W[a][1] - W[b][1], W[a][2] - W[b][2])
    # --- faces: F triangles (F is in the header: cidx after the materials + 36 bytes) of 3 u16 wedge indices, no count of their own
    mat_end = Reader(d).__class__  # placeholder to keep linters quiet
    rr = Reader(d); rr.p = 51; nm_ = rr.cidx()
    for _ in range(nm_):
        rr.cidx()
    rr.p += 36
    F = rr.cidx()
    if not NP * 0.3 <= F <= NP * 8:
        raise ValueError('implausible face count %d' % F)
    faces = None
    # the true start is the one for which EVERY triangle is valid (wedge indices < NW, distinct, UV-coherent); scan all offsets, early exit on the first bad one
    cands = []
    for s_ in (range(pend + 4, wed[0] - 6 * F + 1) if STRICT_FACES else ()):
        ok = True
        for i in range(F):
            a, b, c = struct.unpack_from('<3H', d, s_ + 6 * i)
            if max(a, b, c) >= NW or a == b or b == c or a == c:             # indices only: UV-spanning triangles are legitimate (wrapped islands)
                ok = False; break
        if ok:
            cands.append(s_)
            break
    best = (F, cands[0]) if cands else None
    if not best:                                       # fall back to the tolerant search (a few UV-distant triangles)
        cands = []
        for s_ in range(pend + 4, wed[0] - 6 * F + 1):
            a, b, c = struct.unpack_from('<3H', d, s_)
            if max(a, b, c) >= NW or len({a, b, c}) < 3 or max(uvd(a, b), uvd(b, c), uvd(c, a)) > 0.3:
                continue
            ok = True; coh = 0; tot = 0
            for i in range(0, F, max(1, F // 200)):
                a, b, c = struct.unpack_from('<3H', d, s_ + 6 * i)
                if max(a, b, c) >= NW:
                    ok = False; break
                tot += 1
                if max(uvd(a, b), uvd(b, c), uvd(c, a)) < 0.3:
                    coh += 1
            if ok and coh >= tot * 0.9:
                cands.append(s_)
                if len(cands) >= 600:
                    break
        for s_ in cands:
            good = 0
            for i in range(F):
                a, b, c = struct.unpack_from('<3H', d, s_ + 6 * i)
                if max(a, b, c) < NW and max(uvd(a, b), uvd(b, c), uvd(c, a)) < 0.35:
                    good += 1
            if best is None or good > best[0]:
                best = (good, s_)
                if good == F:
                    break
    if not best or best[0] < F * 0.97:
        raise ValueError('faces not found (F=%d, best %s)' % (F, best))
    faces = (best[1], best[1], F)
    out['faces'] = [struct.unpack_from('<3H', d, faces[1] + 6 * i) for i in range(F)]
    # --- sections (one per material): the section table sits between the points and the faces; each section lists its wedge count,
    #     so the split is the wedge-disjoint cut whose wedge counts all occur in that table
    out['sections'] = split_sections(d, pend, faces[1], out['faces'], len(out['materials']))
    # --- influences
    infl = None
    for p in range(pend, len(d) - 8 * NP):
        n, q = _cidx_at(d, p)
        if n is None or n < NP or n > 8 * NP or q + 8 * n > len(d):
            continue
        ok = True
        for i in list(range(0, min(n, 8))) + list(range(0, n, max(1, n // 100))):
            w, pt, bi = struct.unpack_from('<fHH', d, q + 8 * i)
            if not (0.0 < w <= 1.0001) or pt >= NP or bi >= NB:
                ok = False; break
        if not ok:
            continue
        sums = {}
        for i in range(n):
            w, pt, bi = struct.unpack_from('<fHH', d, q + 8 * i)
            sums[pt] = sums.get(pt, 0.0) + w
        good = sum(1 for s in sums.values() if 0.97 <= s <= 1.03)
        if len(sums) >= NP * 0.95 and good >= len(sums) * 0.9:
            infl = (p, q, n); break
    if not infl:
        raise ValueError('influences not found')
    out['infl'] = [struct.unpack_from('<fHH', d, infl[1] + 8 * i) for i in range(infl[2])]
    out['offsets'] = dict(skeleton=sk_start, points=pstart, wedges=wed[0], faces=faces[0], infl=infl[0])
    return out


def split_sections(d, hstart, hend, F, nmat):
    n = len(F)
    if nmat <= 1:
        return [(0, n)]
    hdr = set()
    for off in range(hstart, hend - 1):
        hdr.add(struct.unpack_from('<H', d, off)[0])
    first = {}; last = {}
    for i, f in enumerate(F):
        for w in f:
            first.setdefault(w, i); last[w] = i
    cover = [0] * (n + 2)
    for w in first:
        cover[first[w] + 1] += 1
        cover[last[w] + 1] -= 1
    ov = 0; zero = []
    for k in range(1, n):
        ov += cover[k]
        if ov == 0:
            zero.append(k)
    wedge_sets = None

    def wcount(a, b):
        s = set()
        for i in range(a, b):
            s.update(F[i])
        return len(s)
    if nmat == 2:
        for k in zero:
            if wcount(0, k) in hdr and wcount(k, n) in hdr:
                return [(0, k), (k, n - k)]
        # sections that share a few wedges: the table lists each section's face count, so take the cut whose two face counts are both
        # listed there and that shares the fewest wedges
        ov2 = 0; overlap = {}
        for k in range(1, n):
            ov2 += cover[k]; overlap[k] = ov2
        cands = sorted((overlap[k], k) for k in hdr if 0 < k < n and (n - k) in hdr)
        if cands:
            k = cands[0][1]
            return [(0, k), (k, n - k)]
    elif nmat == 3:
        for i1, k1 in enumerate(zero):
            c1 = wcount(0, k1)
            if c1 not in hdr:
                continue
            for k2 in zero[i1 + 1:]:
                if wcount(k1, k2) in hdr and wcount(k2, n) in hdr:
                    return [(0, k1), (k1, k2 - k1), (k2, n - k2)]
    return None

def parse_anim(pkg, e):
    d = pkg.read_obj(e)
    r = Reader(d); r.p = 5
    nb = r.cidx()
    bones = []
    for _ in range(nb):
        ni = r.cidx(); r.u32(); par = r.i32()
        bones.append((pkg.names[ni], par))
    # Moves: cidx count, then per chunk: int extra, RootSpeed3D(12), TrackTime f32, StartBone i32, Flags u32,
    #        BoneIndices (cidx n + n x i32), AnimTracks (cidx n + tracks), RootTrack (flags u32 + 3 empty cidx arrays)
    r.u8() if d[r.p] == 1 and False else None
    # the count is located right before the first chunk: find it by trying a few positions after the RefBones
    chunks = None
    for skip in range(0, 12):
        r.p = 6 + 9 * nb + skip if False else None or r.p
    base = None
    # position right after RefBones
    r2 = Reader(d); r2.p = 5; r2.cidx()
    for _ in range(nb):
        r2.cidx(); r2.u32(); r2.i32()
    refend = r2.p
    for skip in range(0, 12):
        r3 = Reader(d); r3.p = refend + skip
        try:
            n = r3.cidx()
        except Exception:
            continue
        if not 1 <= n <= 5000:
            continue
        r3.p += 0
        # try to parse n chunks starting here; accept when all parse and the sequence table follows
        try:
            res = _parse_chunks(d, r3.p, n, nb)
        except Exception:
            continue
        if res:
            ch, end = res
            try:
                rr = Reader(d); rr.p = end; nseq = rr.cidx()
            except Exception:
                continue
            if nseq == n:
                chunks = ch
                break
    if chunks is None:
        raise ValueError('motion chunks not found')
    # sequence table: name, groups, start, frames, notifies (time f32, function cidx, object cidx, 4 bytes), rate, then a block of zeros/flags whose size
    # depends on the package version (33 bytes in the cape sets, 41 in the body sets); the right one is the one that parses to the end of the object
    seqs = None
    for extra in (33, 41, 37, 45):
        try:
            r = Reader(d); r.p = end
            ns = r.cidx()
            if ns != len(chunks):
                continue
            cand = []
            for _ in range(ns):
                r.i32()                                  # leading int (always 0 so far)
                ni = r.cidx()
                if not 0 <= ni < len(pkg.names):
                    raise ValueError('name index')
                ng = r.cidx(); [r.cidx() for _ in range(ng)]
                start = r.i32(); frames = r.i32()
                nn = r.cidx()
                if nn > 400:
                    raise ValueError('notify count')
                for _k in range(nn):
                    r.f32(); r.cidx(); r.cidx(); r.p += 4
                rate = r.f32()
                if not (0.0 < rate < 1000.0) or not 0 <= frames < 100000:
                    raise ValueError('sequence fields')
                r.p += extra
                cand.append(dict(name=pkg.names[ni], start=start, frames=frames, rate=rate))
            if r.p <= len(d) and len(d) - r.p < 64:
                seqs = cand; break
        except Exception:
            continue
    if seqs is None:
        raise ValueError('sequence table not parsed (notifies / extra size)')
    return dict(name=e['name'], bones=bones, chunks=chunks, seqs=seqs)


def _parse_chunks(d, p, n, nb):
    r = Reader(d); r.p = p
    chunks = []
    for _ in range(n):
        r.i32()
        r.p += 12
        tt = r.f32(); r.i32(); r.u32()
        nbi = r.cidx()
        idx = [r.i32() for _ in range(nbi)]
        if not (0 <= nbi <= nb) or any(not 0 <= i < nb for i in idx):
            return None
        ntr = r.cidx()
        if nbi == 0 and ntr == nb:                    # empty index list = every bone, in order
            idx = list(range(nb))
        elif ntr != nbi:
            return None
        tracks = []
        for t in range(ntr):
            fl = r.u32()
            nq = r.cidx(); q0 = r.p; r.p += 16 * nq
            npos = r.cidx(); p0 = r.p; r.p += 12 * npos
            nk = r.cidx(); k0 = r.p; r.p += 4 * nk
            if r.p > len(d):
                return None
            tracks.append(dict(bone=idx[t],
                quat=[struct.unpack_from('<4f', d, q0 + 16 * i) for i in range(nq)],
                pos=[struct.unpack_from('<3f', d, p0 + 12 * i) for i in range(npos)],
                times=[struct.unpack_from('<f', d, k0 + 4 * i)[0] for i in range(nk)]))
        r.u32(); r.cidx(); r.cidx(); r.cidx()      # empty RootTrack
        chunks.append(dict(frames=int(round(tt)), tracks=tracks))
    return chunks, r.p

if __name__ == '__main__':
    pk = Package(sys.argv[1])
    kind = sys.argv[2]
    if kind == 'mesh':
        ok = bad = 0
        for e in pk.exports:
            if pk.cls_name(e['cls']) != 'SkeletalMesh':
                continue
            if len(sys.argv) > 3 and sys.argv[3] not in e['name']:
                continue
            try:
                m = parse_mesh(pk, e)
                ok += 1
                print('OK  %-42s pts %5d wedges %5d faces %5d infl %5d bones %2d' % (e['name'], len(m['points']), len(m['wedges']), len(m['faces']), len(m['infl']), len(m['bones'])))
            except Exception as ex:
                bad += 1
                print('ERR %-42s %s' % (e['name'], ex))
        print('ok', ok, 'bad', bad)
    else:
        for e in pk.exports:
            if pk.cls_name(e['cls']) != 'MeshAnimation':
                continue
            if len(sys.argv) > 3 and sys.argv[3] not in e['name']:
                continue
            try:
                a = parse_anim(pk, e)
                names = [s['name'] for s in a['seqs']]
                print('OK  %-24s bones %d chunks %d seqs %d  frames-match %d' % (e['name'], len(a['bones']), len(a['chunks']), len(a['seqs']),
                      sum(1 for s, c in zip(a['seqs'], a['chunks']) if s['frames'] == c['frames'])))
            except Exception as ex:
                print('ERR %-24s %r' % (e['name'], ex))
