"""itemname-e.dat, Interlude (c6) layout from L2ClientDat/data/structure/dats/itemname.xml

  python itemname.py <itemname-e.dat> [<id> ...]    -> parse + round-trip check, dump the given ids
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2dat


class R:
    def __init__(self, b): self.b = b; self.p = 0
    def u32(self): v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v
    def i32(self): v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v
    def u8(self): v = self.b[self.p]; self.p += 1; return v

    def ucs(self):                                    # UNICODE: int32 byte length + UTF-16LE (no terminator)
        n = self.i32()
        if n < 0 or n > 8192 or self.p + n > len(self.b): raise ValueError('bad unicode len %d @%d' % (n, self.p - 4))
        v = self.b[self.p:self.p + n].decode('utf-16le'); self.p += n; return v

    def asc(self):                                    # ASCF: compact-index length (incl. NUL) + bytes; negative = UTF-16LE
        b = self.u8(); neg = b & 0x80; n = b & 0x3F
        if b & 0x40:
            sh = 6
            while True:
                b = self.u8(); n |= (b & 0x7F) << sh; sh += 7
                if not (b & 0x80): break
        if neg:
            raw = self.b[self.p:self.p + 2 * n]; self.p += 2 * n
            return ('w', raw.decode('utf-16le').rstrip('\0'))
        raw = self.b[self.p:self.p + n]; self.p += n
        return ('a', raw.rstrip(b'\0').decode('latin1'))


def w_ucs(out, s):
    b = s.encode('utf-16le'); out += struct.pack('<i', len(b)) + b


def w_asc(out, v):
    kind, s = v
    if s == '':                                        # empty string = a single 0 length byte, no terminator
        out.append(0); return
    if kind == 'w':
        n = len(s) + 1; body = (s + '\0').encode('utf-16le'); sign = 0x80
    else:
        body = s.encode('latin1') + b'\0'; n = len(body); sign = 0
    first = sign | (n & 0x3F); rest = n >> 6
    if rest:
        out.append(first | 0x40)
        while True:
            c = rest & 0x7F; rest >>= 7
            out.append(c | (0x80 if rest else 0))
            if not rest: break
    else:
        out.append(first)
    out += body


FIELDS = ['name', 'additionalname', 'description', 'popup', 'set_ids', 'set_bonus_desc', 'set_extra_id', 'set_extra_desc',
          'unknown_1', 'unknown_2', 'set_enchant_count', 'set_enchant_effect']


def parse(raw):
    r = R(raw); count = r.u32(); items = []
    for _ in range(count):
        it = {'id': r.u32(), 'name': r.ucs(), 'additionalname': r.ucs(), 'description': r.asc(), 'popup': r.i32(),
              'set_ids': r.asc(), 'set_bonus_desc': r.asc(), 'set_extra_id': r.asc(), 'set_extra_desc': r.asc(),
              'unknown_1': r.u8(), 'unknown_2': r.u8(), 'set_enchant_count': r.u32(), 'set_enchant_effect': r.asc()}
        items.append(it)
    return items, r.p


def serialize(items):
    out = bytearray(struct.pack('<I', len(items)))
    for it in items:
        out += struct.pack('<I', it['id'])
        w_ucs(out, it['name']); w_ucs(out, it['additionalname']); w_asc(out, it['description'])
        out += struct.pack('<i', it['popup'])
        for k in ('set_ids', 'set_bonus_desc', 'set_extra_id', 'set_extra_desc'): w_asc(out, it[k])
        out += struct.pack('<BBI', it['unknown_1'], it['unknown_2'], it['set_enchant_count'])
        w_asc(out, it['set_enchant_effect'])
    return bytes(out)


def load(path):
    raw = l2dat.decode_payload(open(path, 'rb').read(), strict=True)
    items, end = parse(raw)
    return raw, items, end


if __name__ == '__main__':
    raw, items, end = load(sys.argv[1])
    print('items', len(items), '| parsed', end, 'of', len(raw), '| tail', raw[end:].hex())
    print('round-trip identical:', serialize(items) == raw[:end])
    by = {i['id']: i for i in items}
    for x in map(int, sys.argv[2:]):
        print(x, by.get(x))
