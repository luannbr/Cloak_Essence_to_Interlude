#!/usr/bin/env python3
"""UE2 tagged-property reader for the Essence packages (version 133), enough for the particle emitters of LineageEffect*.u.

  parse(pk, data, start=0) -> dict name -> value
     Float/Int/Byte/Bool -> python numbers, Name -> str, Object -> ref int (use essence_static.resolve),
     Vector/Rotator -> tuple, Color -> (r, g, b, a), other structs -> dict (nested tagged properties; omitted members are absent),
     dynamic arrays of struct (ColorScale ...) -> list of dict, CustomMaterials -> list of refs
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2pkg import Reader

RAW_STRUCTS = {'Vector': '<3f', 'Rotator': '<3i', 'Plane': '<4f', 'Quat': '<4f', 'Scale': '<4f'}
OBJ_ARRAYS = {'CustomMaterials', 'Skins', 'Materials'}


def _size(r, sz):
    s = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(sz)
    if s is not None:
        return s
    return r.u8() if sz == 5 else (r.u16() if sz == 6 else r.u32())


def parse(pk, d, start=0, end=None, depth=0):
    return _parse(pk, d, start, end, depth)[0]


def _parse(pk, d, start, end=None, depth=0):
    """-> (dict, position after the terminating 'None')"""
    r = Reader(d)
    r.p = start
    end = len(d) if end is None else end
    out = {}
    while r.p < end:
        ni = r.cidx()
        if ni < 0 or ni >= len(pk.names):
            raise ValueError('bad property name index %d at %d' % (ni, r.p))
        nm = pk.names[ni]
        if nm == 'None':
            break
        info = r.u8()
        typ = info & 15
        sz = (info >> 4) & 7
        arr = bool(info & 0x80)
        sname = pk.names[r.cidx()] if typ == 10 else None
        size = _size(r, sz)
        idx = None
        if arr and typ != 3:
            b = r.u8()
            idx = b if b < 128 else (((b & 0x3F) << 8) | r.u8() if b & 0xC0 == 0x80 else ((b & 0x3F) << 24) | (r.u8() << 16) | (r.u8() << 8) | r.u8())
        raw = d[r.p:r.p + size]
        val = None
        if typ == 1:
            val = raw[0]
        elif typ == 2:
            val = struct.unpack('<i', raw[:4])[0]
        elif typ == 3:
            val = arr
            size = 0
        elif typ == 4:
            val = struct.unpack('<f', raw[:4])[0]
        elif typ in (5, 8):
            val = Reader(raw).cidx()
        elif typ == 6:
            val = pk.names[Reader(raw).cidx()]
        elif typ == 10:
            if sname in RAW_STRUCTS and size == struct.calcsize(RAW_STRUCTS[sname]):
                val = struct.unpack(RAW_STRUCTS[sname], raw)
            elif sname == 'Color' and size == 4:
                val = (raw[2], raw[1], raw[0], raw[3])
            else:
                val = _parse(pk, d, r.p, r.p + size, depth + 1)[0]
        elif typ == 9:
            rr = Reader(d)
            rr.p = r.p
            n = rr.cidx()
            if nm in OBJ_ARRAYS and nm != 'Materials':
                val = [rr.cidx() for _ in range(n)]
            else:
                val = []
                p = rr.p
                for _ in range(n):
                    el, p = _parse(pk, d, p, r.p + size, depth + 1)
                    val.append(el)
        else:
            val = raw.hex()
        key = nm if idx is None else '%s[%d]' % (nm, idx)
        out[key] = val
        r.p += size
    return out, r.p


def rng(v, default=(0.0, 0.0)):
    """Range struct -> (min, max)"""
    if not isinstance(v, dict):
        return default
    lo = hi = None
    for k, x in v.items():
        if k == 'Min':
            lo = x
        elif k == 'Max':
            hi = x
    return (default[0] if lo is None else lo, default[1] if hi is None else hi)


def rangevec(v, default=((0.0, 0.0), (0.0, 0.0), (0.0, 0.0))):
    """RangeVector struct -> ((minx,maxx),(miny,maxy),(minz,maxz))"""
    if not isinstance(v, dict):
        return default
    return tuple(rng(v.get(a), default[i]) for i, a in enumerate('XYZ'))
