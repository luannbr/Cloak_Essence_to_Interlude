#!/usr/bin/env python3
"""Renders the geometry written by tests/fx_dump.exe (one moment of an effect) with the textures of essence_fx.bin.

  python fx_preview.py <pack> <dump.txt> <out.png> [view: back|side|top]

The camera looks at the character from behind (x left, y front, z up; origin = the effect origin, which sits at the upper back); a grey
stand-in torso shows the scale.  Blend styles follow the hook: 3 additive, 6 brighten, 5 darken, 1 alpha.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image, ImageDraw
from essence_dxt import decode_dxt


def read_pack_textures(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'EFX3', d[:4]
    nt = struct.unpack_from('<I', d, 4)[0]
    p = 20
    texs = []

    def cstr():
        nonlocal p
        e = d.index(b'\0', p)
        s = d[p:e].decode('latin1')
        p = e + 1
        return s
    for _ in range(nt):
        name = cstr()
        fmt, w, h, lv = struct.unpack_from('<IIII', d, p)
        p += 16
        mip0 = None
        for l in range(lv):
            sz = struct.unpack_from('<I', d, p)[0]
            if l == 0:
                mip0 = d[p + 4:p + 4 + sz]
            p += 4 + sz
        texs.append((name, decode_dxt(mip0, w, h, fmt)))
    return texs


def render(pack, dump, out, view='back', size=(640, 640), span=60.0):
    texs = read_pack_textures(pack)
    W, H = size
    img = np.zeros((H, W, 3), np.float32)
    img[:] = (0.05, 0.06, 0.08)
    # stand-in body (origin = effect origin ~ upper back): shoulders +-9 wide, from z=-26 to z=+8, head above
    body = Image.new('RGB', size, (0, 0, 0))
    dr = ImageDraw.Draw(body)

    def sc(x, z):
        return (W / 2 + x * (W / span), H * 0.62 - z * (H / span))
    if view == 'back':
        dr.polygon([sc(-9, 6), sc(9, 6), sc(7, -26), sc(-7, -26)], fill=(70, 72, 80))
        dr.ellipse([sc(-4.5, 14)[0], sc(-4.5, 14)[1], sc(4.5, 6)[0], sc(4.5, 6)[1]], fill=(80, 82, 90))
    img = np.maximum(img, np.asarray(body, np.float32) / 255.0)
    batches = []
    cur = None
    for line in open(dump):
        t = line.split()
        if t[0] == 'B':
            cur = dict(tex=int(t[1]), style=int(t[2]), v=[], idx=[])
            batches.append(cur)
        elif t[0] == 'V':
            cur['v'].append([float(x) for x in t[1:4]] + [int(t[4])] + [float(t[5]), float(t[6])])
        elif t[0] == 'I':
            cur['idx'].append(int(t[1]))
    for b in batches:
        name, tex = texs[b['tex']]
        th, tw = tex.shape[:2]
        V = b['v']
        for i in range(0, len(b['idx']), 3):
            vs = [V[j] for j in b['idx'][i:i + 3]]
            pts = []
            for v in vs:
                if view == 'back':
                    sx, sy = W / 2 - v[0] * (W / span), H * 0.62 - v[2] * (H / span)      # camera right = -x
                elif view == 'side':
                    sx, sy = W / 2 + v[1] * (W / span), H * 0.62 - v[2] * (H / span)
                else:
                    sx, sy = W / 2 - v[0] * (W / span), H * 0.5 - v[1] * (H / span)
                pts.append((sx, sy))
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            x0, x1 = max(0, int(min(xs))), min(W - 1, int(max(xs)) + 1)
            y0, y1 = max(0, int(min(ys))), min(H - 1, int(max(ys)) + 1)
            if x0 >= x1 or y0 >= y1:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax, ay), (bx, by), (cx, cy) = pts
            den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-6:
                continue
            l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / den
            l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / den
            l3 = 1 - l1 - l2
            m = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
            if not m.any():
                continue
            u = l1 * vs[0][4] + l2 * vs[1][4] + l3 * vs[2][4]
            v = l1 * vs[0][5] + l2 * vs[1][5] + l3 * vs[2][5]
            tx = np.clip(((u % 1.0) * tw).astype(int), 0, tw - 1)
            ty = np.clip(((v % 1.0) * th).astype(int), 0, th - 1)
            c = tex[ty, tx].astype(np.float32) / 255.0
            col = vs[0][3]
            ca, cr, cg, cb = ((col >> 24) & 255) / 255.0, ((col >> 16) & 255) / 255.0, ((col >> 8) & 255) / 255.0, (col & 255) / 255.0
            src = c[:, :, :3] * np.array([cr, cg, cb], np.float32)
            reg = img[y0:y1 + 1, x0:x1 + 1]
            mm = m[:, :, None]
            if b['style'] in (3,):
                new = reg + src
            elif b['style'] == 6:
                new = src + reg * (1 - np.clip(src, 0, 1))
            elif b['style'] == 5:
                new = reg * (1 - np.clip(src, 0, 1))
            elif b['style'] == 2:
                new = reg * src * 2
            else:
                al = (c[:, :, 3:4] * ca)
                new = src * al + reg * (1 - al)
            reg[:] = np.where(mm, new, reg)
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out)


if __name__ == '__main__':
    render(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else 'back')
