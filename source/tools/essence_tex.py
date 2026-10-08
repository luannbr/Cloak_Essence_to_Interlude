"""Read UE2 tagged properties and decode textures of the Essence texture packages (Ver121, per-file XOR key).
   python essence_tex.py <utx> <ObjectName> [more...]       -> properties of each object (+ texture header guess)"""
import sys, os, struct
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2pkg import Package, Reader

TYPES = {1: 'Byte', 2: 'Int', 3: 'Bool', 4: 'Float', 5: 'Object', 6: 'Name', 7: 'String', 8: 'Class', 9: 'Array', 10: 'Struct',
         11: 'Vector', 12: 'Rotator', 13: 'Str', 14: 'Map', 15: 'Fixed', 16: 'Delegate'}
SIZES = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}


def objname(pk, ci):
    if ci == 0:
        return 'None'
    if ci < 0:
        return 'imp:' + pk.imports[-ci - 1][3]
    return 'exp:' + pk.exports[ci - 1]['name']


def read_props(pk, d, start=0):
    """returns (dict name -> value, end offset). Tagged properties until 'None'."""
    r = Reader(d); r.p = start
    props = {}
    while True:
        ni = r.cidx()
        nm = pk.names[ni]
        if nm == 'None':
            break
        info = r.u8()
        t = info & 0x0F; sz = (info >> 4) & 7; arr = bool(info & 0x80)
        if t == 10:
            r.cidx()
        if sz in SIZES:
            size = SIZES[sz]
        elif sz == 5:
            size = r.u8()
        elif sz == 6:
            size = r.u16()
        else:
            size = r.u32()
        aidx = None
        if arr and t != 3:
            b = r.u8()
            if b < 128:
                aidx = b
            elif b & 0xC0 == 0x80:
                aidx = ((b & 0x7F) << 8) | r.u8()
            else:
                aidx = ((b & 0x3F) << 24) | (r.u8() << 16) | (r.u8() << 8) | r.u8()
        raw = r.bytes(size) if t != 3 else b''
        if t == 5:
            val = objname(pk, Reader(raw).cidx())
        elif t == 2 and size == 4:
            val = struct.unpack('<i', raw)[0]
        elif t == 4 and size == 4:
            val = struct.unpack('<f', raw)[0]
        elif t == 1:
            val = raw[0] if len(raw) == 1 else raw.hex()
        elif t == 6:
            val = pk.names[Reader(raw).cidx()]
        elif t == 3:
            val = bool(info & 0x80)
        else:
            val = raw.hex()
        key = nm if aidx is None else '%s[%d]' % (nm, aidx)
        props[key] = val
    return props, r.p


if __name__ == '__main__':
    pk = Package(sys.argv[1])
    for name in sys.argv[2:]:
        for e in pk.exports:
            if e['name'] != name:
                continue
            d = pk.read_obj(e)
            print('==', name, pk.cls_name(e['cls']), 'size', len(d))
            props, end = read_props(pk, d)
            for k, v in props.items():
                print('   %-22s = %s' % (k, v))
            print('   props end at', end, 'remaining', len(d) - end)
            print('   next bytes:', d[end:end + 48].hex(' '))
