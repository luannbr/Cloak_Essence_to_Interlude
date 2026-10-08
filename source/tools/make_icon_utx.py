#!/usr/bin/env python3
"""Build an Interlude icon package (Lineage2Ver121, package version 123) from PNG files.

  python make_icon_utx.py --template <Interlude systextures\\icon.utx> --out cloakicons.utx --icon cloak_00=path\\a.png --icon cloak_01=b.png ...

The new package keeps the template's name table and import table (so the property names inside the copied Texture objects stay valid),
appends the new texture names, and contains one export per icon: the template's own 32x32 DXT1 Texture object with its pixel payload replaced.
The result is verified by re-reading it with l2pkg and decoding the pixels back.
"""
import argparse, os, struct, sys, random
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2pkg
from essence_dxt import decode_dxt, FMT_DXT1

HDR = 28


def cidx_bytes(v):
    neg = v < 0; v = abs(v)
    if v < 0x40:
        return bytes([(0x80 if neg else 0) | v])
    out = [(0x80 if neg else 0) | 0x40 | (v & 0x3F)]
    v >>= 6
    while True:
        b = v & 0x7F; v >>= 7
        if v: out.append(b | 0x80)
        else:
            out.append(b); break
    return bytes(out)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def expand565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], dtype=np.float64)


def encode_dxt1(rgba):
    """rgba: (h, w, 4) uint8 with h, w multiples of 4 -> DXT1 bytes. Opaque blocks use the 4-colour mode, blocks with alpha < 128 pixels use 3 colours + transparent."""
    h, w, _ = rgba.shape
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = rgba[by:by + 4, bx:bx + 4].reshape(16, 4).astype(np.float64)
            rgb = blk[:, :3]; alpha = blk[:, 3] >= 128
            if not alpha.any():
                out += struct.pack('<HHI', 0, 1, 0xFFFFFFFF); continue          # fully transparent block (c0 <= c1 -> index 3 = transparent)
            pts = rgb[alpha]
            mean = pts.mean(axis=0)
            if len(pts) > 1 and pts.std(axis=0).max() > 1.0:
                cov = np.cov((pts - mean).T)
                v = np.ones(3)
                for _ in range(8):
                    v = cov @ v; n = np.linalg.norm(v)
                    v = v / n if n > 1e-9 else np.array([1.0, 0.0, 0.0])
                t = (pts - mean) @ v
                lo = mean + v * t.min(); hi = mean + v * t.max()
            else:
                lo = hi = mean
            c0 = rgb565(*np.clip(np.round(hi), 0, 255).astype(int)); c1 = rgb565(*np.clip(np.round(lo), 0, 255).astype(int))
            transparent = not alpha.all()
            if transparent:                                                      # 3-colour mode needs c0 <= c1
                if c0 > c1: c0, c1 = c1, c0
                p = [expand565(c0), expand565(c1), (expand565(c0) + expand565(c1)) / 2]
            else:
                if c0 < c1: c0, c1 = c1, c0
                if c0 == c1:
                    p = [expand565(c0)] * 4
                else:
                    e0, e1 = expand565(c0), expand565(c1)
                    p = [e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3]
            idx = 0
            for i in range(16):
                if transparent and not alpha[i]:
                    k = 3
                else:
                    k = int(np.argmin([np.sum((rgb[i] - q) ** 2) for q in p]))
                idx |= k << (2 * i)
            out += struct.pack('<HHI', c0, c1, idx)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--template', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--icon', action='append', required=True, help='name=file.png (repeat)')
    ap.add_argument('--template-export', default='Armor_Back04')
    ap.add_argument('--key', type=lambda s: int(s, 0), default=None, help='XOR byte of the Lineage2Ver121 container (default: sum of the lower-case file name characters, which is what the engine derives)')
    a = ap.parse_args()

    raw = open(a.template, 'rb').read()
    head = raw[:HDR]
    assert head.decode('utf-16le') == 'Lineage2Ver121', head
    tk = raw[HDR] ^ 0xC1
    dec = bytes(b ^ tk for b in raw[HDR:])
    tag, ver, lic, flags, nname, noff, nexp, eoff, nimp, ioff = struct.unpack_from('<IHHIIIIIII', dec, 0)
    guid_gen = dec[0x24:]
    # template name table, kept verbatim (entries with their flags), so every name index used inside the copied object stays valid
    r = l2pkg.Reader(dec); r.p = noff
    names = []; name_flags = []
    for _ in range(nname):
        n = r.cidx(); s = r.bytes(n); fl = r.u32(); names.append(s); name_flags.append(fl)
    name_tbl_end = r.p
    imports_raw = dec[ioff:eoff]                                                 # imports sit right before the exports in the template
    pk = l2pkg.Package(a.template)
    te = next(e for e in pk.exports if e['name'].lower() == a.template_export.lower())
    tobj = bytearray(pk.read_obj(te))
    assert len(tobj) == 590
    foot = struct.pack('<ii', 32, 32)
    fpos = bytes(tobj).find(foot, 0)
    assert fpos == 580, fpos
    pix0 = fpos - 512
    texture_class = te['cls']; export_flags = te['flags']
    # the mip's TLazyArray stores the ABSOLUTE package position of the end of its data (right before the 10-byte footer); the copy must point into the new file
    skip_old = struct.pack('<I', te['off'] + fpos)
    skip_at = bytes(tobj).find(skip_old)
    assert skip_at >= 0 and bytes(tobj).find(skip_old, skip_at + 1) < 0, 'lazy-array skip position not found exactly once in the template object'

    icons = []
    for spec in a.icon:
        nm, path = spec.split('=', 1)
        im = Image.open(path).convert('RGBA').resize((32, 32), Image.LANCZOS)
        icons.append((nm, np.array(im, dtype=np.uint8)))

    # new names are appended to the template's table
    tpl_flags = name_flags[names.index(te['name'].encode('latin1') + b'\0')] if (te['name'].encode('latin1') + b'\0') in names else 0x00070010
    new_name_index = {}
    for nm, _ in icons:
        key = nm.encode('latin1') + b'\0'
        if key in names:
            raise SystemExit('name %s already exists in the template table' % nm)
        names.append(key); name_flags.append(tpl_flags); new_name_index[nm] = len(names) - 1

    # lay out: header(64) | names | objects | imports | exports
    body = bytearray()
    name_bytes = bytearray()
    for s, fl in zip(names, name_flags):
        name_bytes += cidx_bytes(len(s)) + s + struct.pack('<I', fl)
    objects = []; obj_blob = bytearray()
    base_obj = 64 + len(name_bytes)
    for nm, rgba in icons:
        o = bytearray(tobj)
        o[pix0:pix0 + 512] = encode_dxt1(rgba)
        o[skip_at:skip_at + 4] = struct.pack('<I', base_obj + len(obj_blob) + fpos)          # absolute position (in this file) of the end of the pixel data
        objects.append((nm, base_obj + len(obj_blob), len(o)))
        obj_blob += o
    imp_off = base_obj + len(obj_blob)
    exp_off = imp_off + len(imports_raw)
    exp_blob = bytearray()
    for nm, off, size in objects:
        exp_blob += cidx_bytes(texture_class) + cidx_bytes(0) + struct.pack('<i', 0) + cidx_bytes(new_name_index[nm]) + struct.pack('<I', export_flags) + cidx_bytes(size) + cidx_bytes(off)
    guid = bytes(random.Random(0xC10A4).getrandbits(8) for _ in range(16))
    header = struct.pack('<IHHIIIIIII', tag, ver, lic, flags, len(names), 64, len(objects), exp_off, nimp, imp_off) + guid + struct.pack('<III', 1, len(objects), len(names))
    assert len(header) == 64, len(header)
    plain = header + bytes(name_bytes) + bytes(obj_blob) + imports_raw + bytes(exp_blob)
    k = a.key if a.key is not None else (sum(ord(c) for c in os.path.basename(a.out).lower()) & 0xFF)
    enc = bytes(b ^ k for b in plain)
    assert enc[0] ^ 0xC1 == k, 'the first plaintext byte must be 0xC1 (package tag)'
    with open(a.out, 'wb') as f:
        f.write(head + enc)
    print('wrote %s: %d bytes, %d icons, key 0x%02x' % (a.out, HDR + len(enc), len(icons), k))

    # ---- verification: reopen with the independent reader, decode every pixel block
    from essence_dxt import texture_mip0
    chk = l2pkg.Package(a.out)
    assert chk.ver == ver and chk.exp_count == len(icons) and chk.name_count == len(names), (chk.ver, chk.exp_count, chk.name_count)
    worst = 0.0
    for (nm, rgba), e in zip(icons, chk.exports):
        assert e['name'] == nm and chk.cls_name(e['cls']) == 'Texture', (e['name'], nm)
        obj = chk.read_obj(e)
        assert struct.unpack_from('<I', obj, skip_at)[0] == e['off'] + fpos <= chk.size - HDR, 'lazy array skip position does not point at the end of the pixels inside the file'
        assert obj[fpos:fpos + 8] == foot, 'footer is not where the skip position says'
        back = texture_mip0(chk, e)
        m = rgba[:, :, 3] >= 128
        err = np.abs(back[:, :, :3].astype(int) - rgba[:, :, :3].astype(int)).mean(axis=2)[m]
        worst = max(worst, float(err.mean()) if err.size else 0.0)
    print('verify: structure OK, mean colour error per icon <= %.1f / 255' % worst)


if __name__ == '__main__':
    main()

