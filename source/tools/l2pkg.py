"""Minimal L2 (UE2) Ver111 package reader. Read-only. XOR 0xAC after 28-byte wide header."""
import struct, sys, os

KEY = 0xAC
HDR = 28
_TBL = bytes((i ^ KEY) for i in range(256))


def xor(b: bytes, key: int = KEY) -> bytes:
    return b.translate(_TBL if key == KEY else bytes((i ^ key) for i in range(256)))


class Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def i32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]; self.p += 4; return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]; self.p += 2; return v

    def f32(self):
        v = struct.unpack_from('<f', self.d, self.p)[0]; self.p += 4; return v

    def cidx(self):
        b = self.u8()
        neg = b & 0x80
        v = b & 0x3F
        if b & 0x40:
            sh = 6
            while True:
                b = self.u8()
                v |= (b & 0x7F) << sh
                sh += 7
                if not (b & 0x80):
                    break
        return -v if neg else v

    def bytes(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v


class StreamReader(Reader):
    """Reader that pulls blocks from the decrypted stream on demand (for huge tables)."""

    def __init__(self, pkg, off, block=1 << 18):
        self.pkg = pkg; self.base = off; self.block = block
        super().__init__(pkg._read(off, block))
        self.loaded = len(self.d)

    def _need(self, n):
        while self.p + n > len(self.d):
            more = self.pkg._read(self.base + self.loaded, self.block)
            if not more:
                raise IndexError('eof')
            self.d = self.d + more
            self.loaded += len(more)

    def u8(self):
        self._need(1); return super().u8()

    def u32(self):
        self._need(4); return super().u32()

    def i32(self):
        self._need(4); return super().i32()

    def bytes(self, n):
        self._need(n); return super().bytes(n)


class Package:
    def __init__(self, path, names_only=False):
        self.names_only = names_only
        self.path = path
        self.f = open(path, 'rb')
        self.size = os.path.getsize(path)
        head = self.f.read(HDR)
        self.magic = head.decode('utf-16le', 'replace')
        self.enc = self.magic.startswith('Lineage2Ver')
        self.key = KEY
        if self.enc:                                   # Ver111 uses 0xAC; Ver121 uses a per-file byte: package tag 0x9E2A83C1 gives it away
            first = self.f.read(1)[0] if True else 0
            self.key = first ^ 0xC1
        h = self._read(0, 0x40)
        r = Reader(h)
        self.tag = r.u32()
        self.ver = r.u16(); self.lic = r.u16()
        self.flags = r.u32()
        self.name_count = r.u32(); self.name_off = r.u32()
        self.exp_count = r.u32(); self.exp_off = r.u32()
        self.imp_count = r.u32(); self.imp_off = r.u32()
        self._load_tables()

    def _read(self, off, n):
        """off relative to decrypted stream (after wide header)."""
        self.f.seek(HDR + off)
        b = self.f.read(n)
        return xor(b, self.key) if self.enc else b

    def _load_tables(self):
        # names: parsed as a stream so big gaps between tables never get fully read
        if self.name_count > 2000000 or self.name_off >= self.size:
            raise ValueError('bad header')
        r = StreamReader(self, self.name_off)
        self.names = []
        for _ in range(self.name_count):
            n = r.cidx()
            if n < 0:                                   # UTF-16 name: the length is the negated character count
                s = r.bytes(-n * 2).decode('utf-16le', 'replace').rstrip('\x00')
                fl = r.u32()
                self.names.append(s)
                continue
            s = r.bytes(n)
            fl = r.u32()
            self.names.append(s.rstrip(b'\x00').decode('latin1'))
        if self.names_only:
            self.imports = []; self.exports = []
            return
        # imports
        ends = sorted(x for x in (self.name_off, self.exp_off, self.imp_off) if x > self.imp_off)
        end = ends[0] if ends else self.size - HDR
        r = Reader(self._read(self.imp_off, end - self.imp_off))
        self.imports = []
        for _ in range(self.imp_count):
            cp = r.cidx(); cn = r.cidx(); pk = r.i32(); on = r.cidx()
            self.imports.append((self.names[cp], self.names[cn], pk, self.names[on]))
        # exports
        ends = sorted(x for x in (self.name_off, self.exp_off, self.imp_off) if x > self.exp_off)
        end = ends[0] if ends else self.size - HDR
        r = Reader(self._read(self.exp_off, end - self.exp_off))
        self.exports = []
        for _ in range(self.exp_count):
            ci = r.cidx(); si = r.cidx(); pk = r.i32(); on = r.cidx()
            fl = r.u32(); sz = r.cidx()
            off = r.cidx() if sz > 0 else 0
            self.exports.append(dict(cls=ci, sup=si, pkg=pk, name=self.names[on], flags=fl, size=sz, off=off))

    def cls_name(self, ci):
        if ci == 0:
            return 'Class'
        if ci < 0:
            return self.imports[-ci - 1][3]
        return self.exports[ci - 1]['name']

    def read_obj(self, e):
        return self._read(e['off'], e['size'])


if __name__ == '__main__':
    p = Package(sys.argv[1])
    print('magic', p.magic, 'tag %08x' % p.tag, 'ver', p.ver, 'lic', p.lic, 'flags', hex(p.flags))
    print('names', p.name_count, 'exports', p.exp_count, 'imports', p.imp_count)
    print('name_off %d exp_off %d imp_off %d size %d' % (p.name_off, p.exp_off, p.imp_off, p.size))
