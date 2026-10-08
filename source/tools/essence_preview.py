"""Render a contact sheet of an Essence mantle animating (software rasteriser, flat shaded, no textures).
   python essence_preview.py <ukx> <MeshName> <AnimName> <SeqName> <out.png> [nframes=8]"""
import sys, os, math
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence import Package, parse_mesh, parse_anim
from essence_skin import quat_mat, global_transforms, bind_locals


def anim_pose(anim, seq_index, frame):
    """Global (R, t) per animation bone for a (fractional) frame of a sequence; quaternions are conjugated (verified convention)."""
    ch = anim['chunks'][seq_index]
    bones = anim['bones']
    n = len(bones)
    locs = [None] * n
    for tr in ch['tracks']:
        b = tr['bone']
        q = tr['quat']; times = tr['times']
        if len(q) == 1:
            qq = q[0]
        else:
            f = max(0.0, min(frame, times[-1]))
            k = 0
            while k + 1 < len(times) and times[k + 1] <= f:
                k += 1
            k2 = min(k + 1, len(q) - 1)
            span = times[k2] - times[k]
            a = (f - times[k]) / span if span > 1e-6 else 0.0
            q0 = np.array(q[k]); q1 = np.array(q[k2])
            if np.dot(q0, q1) < 0:
                q1 = -q1
            qq = q0 * (1 - a) + q1 * a
            qq = qq / np.linalg.norm(qq)
        pos = tr['pos'][0] if tr['pos'] else (0, 0, 0)
        locs[b] = (quat_mat(tuple(qq), conj=(b != 0)), np.array(pos, float))
    for i in range(n):
        if locs[i] is None:
            locs[i] = (np.eye(3), np.zeros(3))
    G = global_transforms([b[1] for b in bones], locs)
    return {bones[i][0].lower(): G[i] for i in range(n)}


def skin(mesh, bind_G, pose_by_name):
    """Skin the points. bind_G[i] = (R, t) of mesh bone i; pose_by_name maps lower-case bone name -> animated (R, t)."""
    P = np.array(mesh['points'])
    out = np.zeros_like(P)
    names = [b[0].lower() for b in mesh['bones']]
    wsum = np.zeros(len(P))
    for w, pt, bi in mesh['infl']:
        Rb, tb = bind_G[bi]
        a = pose_by_name.get(names[bi])
        if a is None:
            v = P[pt]
        else:
            Ra, ta = a
            v = Ra @ (Rb.T @ (P[pt] - tb)) + ta
        out[pt] += w * v; wsum[pt] += w
    out[wsum == 0] = P[wsum == 0]
    return out


def render(points, mesh, size=(360, 420), view='side', zr=(-12, 62), hr=(-40, 40), tex_color=(70, 90, 140)):
    img = Image.new('RGB', size, (28, 30, 36))
    dr = ImageDraw.Draw(img)
    W = mesh['wedges']
    sx = size[0] / (hr[1] - hr[0]); sy = size[1] / (zr[1] - zr[0])
    s = min(sx, sy)

    def proj(p):
        if view == 'side':
            h = p[1]; dep = p[0]
        else:
            h = -p[0]; dep = p[1]
        return ((h - hr[0]) * s + 8, size[1] - 8 - (p[2] - zr[0]) * s), dep
    # ground and spine reference
    g0, _ = proj((0, 0, 0)); dr.line([(0, g0[1]), (size[0], g0[1])], fill=(70, 70, 80))
    a0, _ = proj((0, 0, 20)); a1, _ = proj((0, 0, 58)); dr.line([a0, a1], fill=(90, 70, 70), width=3)   # spine reference
    tris = []
    for a, b, c in mesh['faces']:
        pa, pb, pc = points[W[a][0]], points[W[b][0]], points[W[c][0]]
        n = np.cross(pb - pa, pc - pa); ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n = n / ln
        (xa, da), (xb, db), (xc, dc) = proj(pa), proj(pb), proj(pc)
        tris.append(((da + db + dc) / 3, (xa, xb, xc), n))
    tris.sort(key=lambda t: t[0])                       # painter's algorithm: far to near (dep grows towards the viewer for the side view)
    light = np.array([0.4, -0.5, 0.75]); light /= np.linalg.norm(light)
    for _, (A, B, C), n in tris:
        sh = 0.35 + 0.65 * abs(float(np.dot(n, light)))
        col = tuple(int(c * sh) for c in tex_color)
        dr.polygon([A, B, C], fill=col, outline=None)
    return img


def main():
    pk = Package(sys.argv[1])
    me = next(x for x in pk.exports if x['name'] == sys.argv[2])
    ae = next(x for x in pk.exports if x['name'] == sys.argv[3])
    seq = sys.argv[4]; out = sys.argv[5]; nf = int(sys.argv[6]) if len(sys.argv) > 6 else 8
    mesh = parse_mesh(pk, me); anim = parse_anim(pk, ae)
    si = next(i for i, s in enumerate(anim['seqs']) if s['name'].lower() == seq.lower())
    frames = anim['seqs'][si]['frames']
    parents = [b[1] for b in mesh['bones']]
    bind_G = global_transforms(parents, bind_locals(mesh['bones'], False, True))
    print('mesh %s: %d points %d faces; anim %s seq %s (%d frames)' % (mesh['name'], len(mesh['points']), len(mesh['faces']), anim['name'], seq, frames))
    sheets = []
    for k in range(nf):
        f = (frames - 1) * k / max(1, nf - 1) if frames > 1 else 0
        pose = anim_pose(anim, si, f)
        pts = skin(mesh, bind_G, pose)
        side = render(pts, mesh, view='side'); back = render(pts, mesh, view='back')
        d = ImageDraw.Draw(side); d.text((6, 4), 'frame %.0f/%d  (side)' % (f, frames), fill=(220, 220, 220))
        sheets.append((side, back))
    # bind pose for reference
    bp = np.array(mesh['points'])
    ref = render(bp, mesh, view='side'); ImageDraw.Draw(ref).text((6, 4), 'bind pose', fill=(220, 220, 220))
    w, h = sheets[0][0].size
    cols = 4
    rows = math.ceil(len(sheets) / cols)
    sheet = Image.new('RGB', (cols * w, (rows * 2 + 1) * h), (20, 20, 24))
    sheet.paste(ref, (0, 0)); sheet.paste(render(bp, mesh, view='back'), (w, 0))
    for i, (s, b) in enumerate(sheets):
        c = i % cols; r = i // cols
        sheet.paste(s, (c * w, (1 + 2 * r) * h)); sheet.paste(b, (c * w, (2 + 2 * r) * h))
    sheet.save(out)
    print('saved', out, sheet.size)


if __name__ == '__main__':
    main()
