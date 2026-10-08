"""DXT1/3/5 decoding + material lookup (FinalBlend -> Shader -> Diffuse Texture) for the Essence texture packages."""
import os, sys, struct
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2pkg import Package, Reader
from essence_tex import read_props

FMT_DXT1, FMT_RGBA8, FMT_DXT3, FMT_DXT5 = 3, 5, 7, 8


def _rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def decode_dxt(data, w, h, fmt):
    bw, bh = w // 4, h // 4
    bs = 8 if fmt == FMT_DXT1 else 16
    blk = np.frombuffer(data, np.uint8, count=bw * bh * bs).reshape(bh, bw, bs)
    co = blk[:, :, bs - 8:]
    c0 = co[:, :, 0].astype(np.uint16) | (co[:, :, 1].astype(np.uint16) << 8)
    c1 = co[:, :, 2].astype(np.uint16) | (co[:, :, 3].astype(np.uint16) << 8)
    col0 = _rgb565(c0).astype(np.int32); col1 = _rgb565(c1).astype(np.int32)
    four = (c0 > c1) | (fmt != FMT_DXT1)
    col2 = np.where(four[..., None], (2 * col0 + col1) // 3, (col0 + col1) // 2)
    col3 = np.where(four[..., None], (col0 + 2 * col1) // 3, 0)
    pal = np.stack([col0, col1, col2, col3], axis=2)                      # (bh, bw, 4, 3)
    bits = (co[:, :, 4].astype(np.uint32) | (co[:, :, 5].astype(np.uint32) << 8) | (co[:, :, 6].astype(np.uint32) << 16) | (co[:, :, 7].astype(np.uint32) << 24))
    sh = (np.arange(16, dtype=np.uint32) * 2)
    idx = ((bits[:, :, None] >> sh[None, None, :]) & 3).astype(np.int64)  # (bh, bw, 16)
    rgb = np.take_along_axis(pal, idx[..., None].repeat(3, axis=3), axis=2)  # (bh, bw, 16, 3)
    alpha = np.full((bh, bw, 16), 255, np.int32)
    if fmt == FMT_DXT1:
        alpha = np.where((~four)[..., None] & (idx == 3), 0, 255)
    elif fmt == FMT_DXT3:
        a = blk[:, :, :8]
        lo = a & 0x0F; hi = a >> 4
        nib = np.stack([lo, hi], axis=3).reshape(bh, bw, 16)
        alpha = (nib.astype(np.int32) * 17)
    elif fmt == FMT_DXT5:
        a0 = blk[:, :, 0].astype(np.int32); a1 = blk[:, :, 1].astype(np.int32)
        ab = np.zeros((bh, bw), np.uint64)
        for i in range(6):
            ab |= blk[:, :, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        ai = ((ab[:, :, None] >> (np.arange(16, dtype=np.uint64) * np.uint64(3))[None, None, :]) & np.uint64(7)).astype(np.int64)
        tab = np.zeros((bh, bw, 8), np.int32)
        tab[:, :, 0] = a0; tab[:, :, 1] = a1
        m = a0 > a1
        for k in range(1, 7):
            tab[:, :, k + 1] = np.where(m, ((7 - k) * a0 + k * a1) // 7, np.where(k <= 4, ((5 - k) * a0 + k * a1) // 5, 0))
        tab[:, :, 6] = np.where(m, tab[:, :, 6], 0); tab[:, :, 7] = np.where(m, tab[:, :, 7], 255)
        alpha = np.take_along_axis(tab, ai, axis=2)
    out = np.concatenate([rgb.astype(np.int32), alpha[..., None]], axis=3)   # (bh, bw, 16, 4)
    out = out.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4)
    return out.astype(np.uint8)


def texture_mip0(pk, e):
    d = pk.read_obj(e)
    props, pend = read_props(pk, d)
    w, h, fmt = props['USize'], props['VSize'], props['Format']
    if fmt == FMT_RGBA8:
        size = w * h * 4
    elif fmt == FMT_DXT1:
        size = w * h // 2
    else:
        size = w * h
    ub, vb = props.get('UBits', 0), props.get('VBits', 0)
    pat = struct.pack('<ii', w, h) + bytes([ub, vb])
    foot = d.find(pat, pend)
    if foot < 0 or foot - size < pend:
        raise ValueError('mip0 footer not found for %s' % e['name'])
    data = d[foot - size:foot]
    if fmt == FMT_RGBA8:
        a = np.frombuffer(data, np.uint8).reshape(h, w, 4)
        return a[:, :, [2, 1, 0, 3]].copy()
    return decode_dxt(data, w, h, fmt)


class TexPackage:
    def __init__(self, path):
        self.pk = Package(path)
        self.by_group_name = {}
        for i, e in enumerate(self.pk.exports):
            self.by_group_name[(e['pkg'], e['name'].lower())] = i
        self.group_ids = {}
        for i, e in enumerate(self.pk.exports):
            if self.pk.cls_name(e['cls']) == 'Package':
                self.group_ids[e['name'].lower()] = i + 1

    def find(self, group, name):
        g = self.group_ids.get(group.lower(), 0)
        i = self.by_group_name.get((g, name.lower()))
        return None if i is None else self.pk.exports[i]

    def material(self, group, name):
        """-> dict(diffuse=RGBA ndarray, alpha_test=bool, alpha_ref=int, two_sided=bool) for FinalBlend/Shader/Texture `group.name`"""
        e = self.find(group, name)
        if e is None:
            raise KeyError('%s.%s' % (group, name))
        cls = self.pk.cls_name(e['cls'])
        info = dict(alpha_test=False, alpha_ref=0, two_sided=False)
        guard = 0
        while cls != 'Texture' and guard < 6:
            props, _ = read_props(self.pk, self.pk.read_obj(e))
            if cls == 'FinalBlend':
                info['alpha_test'] = bool(props.get('AlphaTest')); info['alpha_ref'] = props.get('AlphaRef', 0); info['two_sided'] = bool(props.get('TwoSided'))
                nxt = props.get('Material')
            elif cls == 'Shader':
                nxt = props.get('Diffuse')
            elif cls == 'Combiner':
                nxt = props.get('Material1')
            else:
                raise ValueError('unsupported material class ' + cls)
            if not nxt or not nxt.startswith('exp:'):
                raise ValueError('material chain broken at %s' % cls)
            e2 = self.find_in_group_of(e, nxt[4:])
            if e2 is None:
                raise KeyError(nxt)
            e = e2; cls = self.pk.cls_name(e['cls']); guard += 1
        info['diffuse'] = texture_mip0(self.pk, e)
        info['texture'] = e['name']
        return info

    def find_in_group_of(self, e, name):
        i = self.by_group_name.get((e['pkg'], name.lower()))
        if i is not None:
            return self.pk.exports[i]
        for (g, n), i in self.by_group_name.items():
            if n == name.lower():
                return self.pk.exports[i]
        return None


if __name__ == '__main__':
    from PIL import Image
    tp = TexPackage(sys.argv[1])
    m = tp.material(sys.argv[2], sys.argv[3])
    print('texture', m['texture'], m['diffuse'].shape, {k: v for k, v in m.items() if k not in ('diffuse',)})
    Image.fromarray(m['diffuse']).save(sys.argv[4])
