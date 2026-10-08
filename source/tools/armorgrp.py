"""Reader/writer for armorgrp.dat, Interlude (c6) layout from L2ClientDat/data/structure/dats/armorgrp.xml.

  python armorgrp.py <armorgrp.dat>            -> parse + round-trip check + summary
"""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
import l2dat

RACES = ['HumnFigh', 'DarkElf', 'Dorf', 'Elf', 'HumnMyst', 'OrcFigh', 'OrcMage']   # order in the file
# L2 body names used by the cloak meshes -> armorgrp race key
BODY_TO_KEY = {'MFighter': 'm_HumnFigh', 'FFighter': 'f_HumnFigh', 'MDarkElf': 'm_DarkElf', 'FDarkElf': 'f_DarkElf',
               'MDwarf': 'm_Dorf', 'FDwarf': 'f_Dorf', 'MElf': 'm_Elf', 'FElf': 'f_Elf', 'MMagic': 'm_HumnMyst',
               'FMagic': 'f_HumnMyst', 'MOrc': 'm_OrcFigh', 'FOrc': 'f_OrcFigh', 'MShaman': 'm_OrcMage', 'FShaman': 'f_OrcMage'}
MTX_KEYS = []
for r in RACES:
    for g in ('m', 'f'):
        MTX_KEYS += ['%s_%s' % (g, r), '%s_%s_add' % (g, r)]
MTX_TAIL = ['Unknown_MT', 'NPC', 'AAC']


class R:
    def __init__(self, b, p=0):
        self.b = b; self.p = p

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v

    def s(self):                                      # UNICODE: int32 byte length + UTF-16LE (no terminator)
        n = self.i32()
        if n < 0 or n > 4096 or self.p + n > len(self.b):
            raise ValueError('bad string length %d at %d' % (n, self.p - 4))
        v = self.b[self.p:self.p + n].decode('utf-16le'); self.p += n; return v

    def mtx(self):
        a = [self.s() for _ in range(self.i32())]
        t = [self.s() for _ in range(self.i32())]
        return {'mesh': a, 'tex': t}


def parse(raw):
    r = R(raw)
    count = r.u32()
    items = []
    for _ in range(count):
        it = {}
        for k in ('tag', 'object_id', 'drop_type', 'drop_anim_type', 'drop_radius', 'drop_height', 'UNK_0'):
            it[k] = r.u32()
        it['drop_mesh'] = [r.s() for _ in range(3)]
        it['drop_texture'] = [r.s() for _ in range(3)]
        it['icon'] = [r.s() for _ in range(5)]
        it['durability'] = r.i32()
        for k in ('weight', 'material_type', 'crystallizable', 'UNK_1', 'body_part'):
            it[k] = r.u32()
        for k in MTX_KEYS + MTX_TAIL:
            it[k] = r.mtx()
        it['attack_effect'] = r.s()
        it['item_sound'] = [r.s() for _ in range(r.u32())]
        it['drop_sound'] = r.s(); it['equip_sound'] = r.s()
        for k in ('UNK_2', 'UNK_3', 'armor_type', 'crystal_type', 'avoid_mod', 'pdef', 'mdef', 'mpbonus'):
            it[k] = r.u32()
        items.append(it)
    return items, r.p


def ws(out, s):
    b = s.encode('utf-16le'); out += struct.pack('<i', len(b)) + b


def wmtx(out, m):
    out += struct.pack('<i', len(m['mesh']))
    for s in m['mesh']: ws(out, s)
    out += struct.pack('<i', len(m['tex']))
    for s in m['tex']: ws(out, s)


def write_item(out, it):
    for k in ('tag', 'object_id', 'drop_type', 'drop_anim_type', 'drop_radius', 'drop_height', 'UNK_0'):
        out += struct.pack('<I', it[k])
    for k in ('drop_mesh', 'drop_texture', 'icon'):
        for s in it[k]: ws(out, s)
    out += struct.pack('<i', it['durability'])
    for k in ('weight', 'material_type', 'crystallizable', 'UNK_1', 'body_part'):
        out += struct.pack('<I', it[k])
    for k in MTX_KEYS + MTX_TAIL:
        wmtx(out, it[k])
    ws(out, it['attack_effect'])
    out += struct.pack('<I', len(it['item_sound']))
    for s in it['item_sound']: ws(out, s)
    ws(out, it['drop_sound']); ws(out, it['equip_sound'])
    for k in ('UNK_2', 'UNK_3', 'armor_type', 'crystal_type', 'avoid_mod', 'pdef', 'mdef', 'mpbonus'):
        out += struct.pack('<I', it[k])


def serialize(items):
    out = bytearray(struct.pack('<I', len(items)))
    for it in items:
        write_item(out, it)
    return bytes(out)


def load(path):
    raw = l2dat.decode_payload(open(path, 'rb').read(), strict=False)
    items, end = parse(raw)
    return raw, items, end


if __name__ == '__main__':
    raw, items, end = load(sys.argv[1])
    print('items', len(items), '| parsed bytes', end, 'of', len(raw), '| remaining', len(raw) - end)
    ser = serialize(items)
    print('round-trip identical to parsed region:', ser == raw[:end])
    bp = {}
    for it in items:
        bp[it['body_part']] = bp.get(it['body_part'], 0) + 1
    print('body_part histogram:', dict(sorted(bp.items())))
