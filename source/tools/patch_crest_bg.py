#!/usr/bin/env python3
"""Rewrites the crest-window background textures (<name>_t02_bg_tint) of the clan cloaks in an essence_cloth.bin so that the window matches the fabric around it.

  python patch_crest_bg.py <in.bin> <out.bin> [--key] [--scale 0.8] [--logo <regex on the bg texture name>=<image path> ...]   (--key: the flat background colour of the picture becomes transparent)

The window of the clan cloak mesh (Hsm_ad00) sits on the cloth texture at u 0.366..0.634, v 0.147..0.398 (the cloth vertices that share a position with the window
triangles).  Where the cloth texture has fabric there (the war / pvp cloaks), that rectangle IS the new background.  Where it is a black hole (growth / combat / economy),
the Essence background picture is colour-transferred (mean and, partly, contrast per channel) onto the fabric ring around the hole.
--logo puts a picture into the window (256x256, an alpha channel is composited over the fabric background; a picture without alpha replaces it).
Only the texture section of the pack changes; sets, collars and looks follow unchanged, so the texture indices stay valid.
"""
import os, sys, struct, io, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image, ImageFilter
from essence_dxt import decode_dxt

U0, U1, V0, V1 = 0.366, 0.634, 0.147, 0.398            # window rectangle in the UV space of the cloth texture
OUT = 256


def read_textures(d):
    assert d[:4] == b'ECL2', d[:4]
    nTex = struct.unpack_from('<I', d, 4)[0]
    p = 20
    texs = []
    for _ in range(nTex):
        s = p
        e = d.index(b'\0', p)
        name = d[p:e].decode('latin1')
        p = e + 1
        fmt, w, h, lv = struct.unpack_from('<IIII', d, p)
        p += 16
        levels = []
        for _l in range(lv):
            sz = struct.unpack_from('<I', d, p)[0]
            levels.append(d[p + 4:p + 4 + sz])
            p += 4 + sz
        texs.append(dict(name=name, fmt=fmt, w=w, h=h, levels=levels, start=s, end=p))
    return texs, p


def dxt1_levels(img, pixel_format='DXT1'):
    im = Image.fromarray(img[:, :, :3], 'RGB')
    w, h = im.size
    out = []
    while True:
        b = io.BytesIO()
        im.save(b, 'DDS', pixel_format=pixel_format)
        out.append(b.getvalue()[128:])
        if w <= 4 or h <= 4:
            break
        w //= 2
        h //= 2
        im = im.resize((w, h), Image.LANCZOS)
    return out


def blob(name, w, h, levels, fmt=3):
    return name.encode('latin1') + b'\0' + struct.pack('<IIII', fmt, w, h, len(levels)) + b''.join(struct.pack('<I', len(l)) + l for l in levels)


def valid_mask(a):
    return (a[:, :, 3] >= 200) & (a[:, :, :3].astype(int).sum(2) >= 30)


def new_background(cloth, bg):
    h, w = cloth.shape[:2]
    x0, x1, y0, y1 = int(U0 * w), int(U1 * w), int(V0 * h), int(V1 * h)
    rect = cloth[y0:y1, x0:x1]
    hole = ~valid_mask(rect)
    frac = float(hole.mean())
    # fabric ring around the rectangle
    m = int(0.05 * w)
    ring = cloth[max(0, y0 - m):y1 + m, max(0, x0 - m):x1 + m].copy()
    rv = valid_mask(ring)
    ry0, rx0 = y0 - max(0, y0 - m), x0 - max(0, x0 - m)
    rv[ry0:ry0 + (y1 - y0), rx0:rx0 + (x1 - x0)] = False
    px = ring[rv][:, :3].astype(float)
    big = lambda a, resample: Image.fromarray(a).resize((OUT, OUT), resample)
    if frac <= 0.02 or len(px) < 200:
        r = np.asarray(big(np.ascontiguousarray(rect[:, :, :3]), Image.LANCZOS)).copy()
        return r, frac, 'cloth rectangle'
    mu_r, sd_r = px.mean(0), px.std(0)
    b = bg[:, :, :3].astype(float).reshape(-1, 3)
    mu_b, sd_b = b.mean(0), np.maximum(b.std(0), 1.0)
    k = np.clip(0.6 * sd_r / sd_b, 0.15, 1.0)                                  # keep part of the background's own contrast
    t = (bg[:, :, :3].astype(float) - mu_b) * k + mu_r
    t = np.clip(t, 0, 255).astype(np.uint8)
    if t.shape[0] != OUT:
        t = np.asarray(Image.fromarray(t).resize((OUT, OUT), Image.LANCZOS))
    if frac < 0.5:                                                             # small hole: fabric where the cloth has it, the transferred background elsewhere
        mk = Image.fromarray((hole * 255).astype(np.uint8)).resize((OUT, OUT), Image.BILINEAR).filter(ImageFilter.GaussianBlur(3))
        mk = np.asarray(mk).astype(float)[:, :, None] / 255.0
        fab = np.asarray(big(np.ascontiguousarray(rect[:, :, :3]), Image.LANCZOS)).astype(float)
        t = np.clip(fab * (1 - mk) + t.astype(float) * mk, 0, 255).astype(np.uint8)
    return t, frac, 'colour transfer (mean %s -> %s)' % (b.mean(0).round().astype(int).tolist(), mu_r.round().astype(int).tolist())


def apply_logo(img, path, key=False, scale=1.0):
    im = Image.open(path).convert('RGBA')
    if scale != 1.0:                                                           # smaller, centred, so the picture does not touch the edge of the window
        n = int(round(OUT * scale))
        canvas = Image.new('RGBA', (OUT, OUT), (0, 0, 0, 0))
        canvas.paste(im.resize((n, n), Image.LANCZOS), ((OUT - n) // 2, (OUT - n) // 2))
        im = canvas
    else:
        im = im.resize((OUT, OUT), Image.LANCZOS)
    a = np.asarray(im).astype(float)
    al = a[:, :, 3:4] / 255.0
    if key:                                                                    # picture on a flat background: the corner colour becomes transparent (soft edge)
        c = a[:6, :6, :3].reshape(-1, 3).mean(0)
        dist = np.sqrt(((a[:, :, :3] - c) ** 2).sum(2))
        al = np.clip((dist - 12.0) / 40.0, 0.0, 1.0)[:, :, None] * al
    return np.clip(a[:, :, :3] * al + img.astype(float) * (1 - al), 0, 255).astype(np.uint8)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    logos = []
    keyed = False
    scale = 1.0
    args = sys.argv[3:]
    while args:
        if args[0] == '--logo' and len(args) > 1:
            rx, _, pth = args[1].partition('=')
            logos.append((rx, pth))
            args = args[2:]
        elif args[0] == '--scale' and len(args) > 1:
            scale = float(args[1])
            args = args[2:]
        elif args[0] == '--key':
            keyed = True
            args = args[1:]
        else:
            raise SystemExit('bad argument ' + args[0])
    d = open(src, 'rb').read()
    texs, tend = read_textures(d)
    byname = {t['name']: t for t in texs}
    out = bytearray(d[:20])
    for t in texs:
        m = re.match(r'^(H_\w+?)_t02_bg_tint$', t['name'])
        cloth = byname.get(m.group(1) + '_t01_ori') if m else None
        if not cloth:
            out += d[t['start']:t['end']]
            continue
        c = decode_dxt(cloth['levels'][0], cloth['w'], cloth['h'], cloth['fmt'])
        bg = decode_dxt(t['levels'][0], t['w'], t['h'], t['fmt'])
        img, frac, how = new_background(c, bg)
        fmt = 3
        for rx, pth in logos:
            if re.search(rx, t['name']):
                img = apply_logo(img, pth, keyed, scale)
                how += ' + logo ' + os.path.basename(pth)
                fmt = 8                                                        # DXT5: sharper edges for a picture
        levels = dxt1_levels(np.ascontiguousarray(img), 'DXT5' if fmt == 8 else 'DXT1')
        out += blob(t['name'], OUT, OUT, levels, fmt)
        print('%-26s hole %.2f -> %s' % (t['name'], frac, how))
    out += d[tend:]
    open(dst, 'wb').write(bytes(out))
    print('wrote', dst, len(out), 'bytes (was %d)' % len(d))


main()
