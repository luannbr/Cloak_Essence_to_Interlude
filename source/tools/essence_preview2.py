"""Textured contact sheet of an Essence mantle animating (z-buffered software rasteriser, alpha-tested like the FinalBlend).
   python essence_preview2.py <mantles.ukx> <tex.utx> <MeshName> <AnimName> <SeqName> <out.png> [nframes=4]"""
import sys, os, math
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence import Package, parse_mesh, parse_anim
from essence_skin import global_transforms, bind_locals
from essence_preview import anim_pose, skin
from essence_dxt import TexPackage

SS = 2                                                    # supersampling


def mesh_materials(pk, mesh, tp):
    mats = []
    for ref in mesh['materials']:
        imp = pk.imports[-ref - 1]
        group = pk.imports[-imp[2] - 1][3]
        mats.append(tp.material(group, imp[3]))
    return mats


def rasterise(points, mesh, mats, size=(360, 420), view='side', zr=(-12, 62), hr=(-40, 40)):
    W, H = size[0] * SS, size[1] * SS
    zbuf = np.full((H, W), np.inf, np.float32)
    img = np.zeros((H, W, 3), np.float32); img[:] = (28, 30, 36)
    hit = np.zeros((H, W), bool)
    s = min(W / (hr[1] - hr[0]), H / (zr[1] - zr[0]))
    wedges = mesh['wedges']
    P = points
    secs = mesh.get('sections') or [(0, len(mesh['faces']))]
    light = np.array([0.35, -0.45, 0.8]); light /= np.linalg.norm(light)

    def proj(p):
        if view == 'side':
            return (p[1] - hr[0]) * s + 8 * SS, H - 8 * SS - (p[2] - zr[0]) * s, -p[0]
        return (-p[0] - hr[0]) * s + 8 * SS, H - 8 * SS - (p[2] - zr[0]) * s, p[1]
    faces = mesh['faces']
    for si, (first, cnt) in enumerate(secs):
        mat = mats[min(si, len(mats) - 1)]
        tex = mat['diffuse']; th, tw = tex.shape[:2]
        aref = mat['alpha_ref'] if mat['alpha_test'] else 0
        for fi in range(first, first + cnt):
            a, b, c = faces[fi]
            pa, pb, pc = P[wedges[a][0]], P[wedges[b][0]], P[wedges[c][0]]
            n = np.cross(pb - pa, pc - pa); ln = np.linalg.norm(n)
            if ln < 1e-9:
                continue
            n /= ln
            (x0, y0, d0), (x1, y1, d1), (x2, y2, d2) = proj(pa), proj(pb), proj(pc)
            minx = int(max(0, math.floor(min(x0, x1, x2)))); maxx = int(min(W - 1, math.ceil(max(x0, x1, x2))))
            miny = int(max(0, math.floor(min(y0, y1, y2)))); maxy = int(min(H - 1, math.ceil(max(y0, y1, y2))))
            if minx > maxx or miny > maxy:
                continue
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-9:
                continue
            xs = np.arange(minx, maxx + 1) + 0.5; ys = np.arange(miny, maxy + 1) + 0.5
            gx, gy = np.meshgrid(xs, ys)
            l0 = ((y1 - y2) * (gx - x2) + (x2 - x1) * (gy - y2)) / den
            l1 = ((y2 - y0) * (gx - x2) + (x0 - x2) * (gy - y2)) / den
            l2 = 1 - l0 - l1
            m = (l0 >= -1e-4) & (l1 >= -1e-4) & (l2 >= -1e-4)
            if not m.any():
                continue
            dep = l0 * d0 + l1 * d1 + l2 * d2
            sub = zbuf[miny:maxy + 1, minx:maxx + 1]
            m &= dep < sub
            if not m.any():
                continue
            u = l0 * wedges[a][1] + l1 * wedges[b][1] + l2 * wedges[c][1]
            v = l0 * wedges[a][2] + l1 * wedges[b][2] + l2 * wedges[c][2]
            tx = np.clip((u * tw).astype(np.int64) % tw, 0, tw - 1); ty = np.clip((v * th).astype(np.int64) % th, 0, th - 1)
            texel = tex[ty, tx]
            if aref:
                m &= texel[..., 3] >= aref
                if not m.any():
                    continue
            sh = 0.45 + 0.55 * abs(float(np.dot(n, light)))
            sub[m] = dep[m]
            region = img[miny:maxy + 1, minx:maxx + 1]
            region[m] = texel[..., :3][m] * sh
            hit[miny:maxy + 1, minx:maxx + 1] |= m
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize(size, Image.LANCZOS)
    return out


def main():
    pk = Package(sys.argv[1]); tp = TexPackage(sys.argv[2])
    me = next(x for x in pk.exports if x['name'] == sys.argv[3])
    ae = next(x for x in pk.exports if x['name'] == sys.argv[4])
    seq = sys.argv[5]; out = sys.argv[6]; nf = int(sys.argv[7]) if len(sys.argv) > 7 else 4
    mesh = parse_mesh(pk, me); anim = parse_anim(pk, ae)
    mats = mesh_materials(pk, mesh, tp)
    si = next(i for i, s in enumerate(anim['seqs']) if s['name'].lower() == seq.lower())
    frames = anim['seqs'][si]['frames']
    bind_G = global_transforms([b[1] for b in mesh['bones']], bind_locals(mesh['bones'], False, True))
    print('%s: %d faces, sections %s, %d materials; anim %s %s (%d frames)' % (mesh['name'], len(mesh['faces']), mesh.get('sections'), len(mats), anim['name'], seq, frames))
    bp = np.array(mesh['points'])
    tiles = [(rasterise(bp, mesh, mats, view='side'), 'bind pose (side)'), (rasterise(bp, mesh, mats, view='back'), 'bind pose (back)')]
    for k in range(nf):
        f = (frames - 1) * k / max(1, nf - 1) if frames > 1 else 0
        pts = skin(mesh, bind_G, anim_pose(anim, si, f))
        tiles.append((rasterise(pts, mesh, mats, view='side'), 'frame %.0f/%d side' % (f, frames)))
        tiles.append((rasterise(pts, mesh, mats, view='back'), 'frame %.0f/%d back' % (f, frames)))
    w, h = tiles[0][0].size
    cols = 4; rows = math.ceil(len(tiles) / cols)
    sheet = Image.new('RGB', (cols * w, rows * h), (20, 20, 24))
    for i, (im, label) in enumerate(tiles):
        ImageDraw.Draw(im).text((6, 4), label, fill=(230, 230, 230))
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    sheet.save(out); print('saved', out, sheet.size)


if __name__ == '__main__':
    main()
