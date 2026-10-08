"""Read the Materials[] of every <Body>_Cloak_<id> SkeletalMesh in a Lineage2Ver111 .ukx (XOR 0xAC).

The materials array sits right after the bounding box/sphere: a compact-index count followed by compact object refs.
Usage: python ukx_materials.py <LineShieldCloaks.ukx>   (prints a summary; import `cloak_materials()` from other tools)
"""
import os, re, struct, sys, collections
sys.path.insert(0, os.path.dirname(__file__))

KEY = 0xAC
HDR = 28


def _x(b): return bytes(c ^ KEY for c in b)


class Pkg:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = _x(self._rd(0, 0x30))
        self.name_count, self.name_off, self.exp_count, self.exp_off, self.imp_count, self.imp_off = struct.unpack_from('<IIIIII', h, 12)
        self._names(); self._imports(); self._exports()

    def _rd(self, off, n):
        self.f.seek(HDR + off); return self.f.read(n)

    class Cur:
        def __init__(s, d, p=0): s.d = d; s.p = p
        def u8(s): v = s.d[s.p]; s.p += 1; return v
        def u32(s): v = struct.unpack_from('<I', s.d, s.p)[0]; s.p += 4; return v
        def i32(s): v = struct.unpack_from('<i', s.d, s.p)[0]; s.p += 4; return v
        def idx(s):
            b = s.u8(); neg = b & 0x80; v = b & 0x3F
            if b & 0x40:
                sh = 6
                while True:
                    b = s.u8(); v |= (b & 0x7F) << sh; sh += 7
                    if not (b & 0x80): break
            return -v if neg else v

    def _names(self):
        end = min(x for x in (self.exp_off, self.imp_off) if x > self.name_off)
        c = Pkg.Cur(_x(self._rd(self.name_off, end - self.name_off)))
        self.names = []
        for _ in range(self.name_count):
            n = c.idx(); s = c.d[c.p:c.p + n]; c.p += n; c.u32()
            self.names.append(s.rstrip(b'\0').decode('latin1'))

    def _imports(self):
        end = min(x for x in (self.exp_off, self.name_off) if x > self.imp_off)
        c = Pkg.Cur(_x(self._rd(self.imp_off, end - self.imp_off)))
        self.imports = []
        for _ in range(self.imp_count):
            c.idx(); cn = c.idx(); c.i32(); on = c.idx()
            self.imports.append(self.names[on])

    def _exports(self):
        c = Pkg.Cur(_x(self._rd(self.exp_off, 4 * 1024 * 1024)))
        self.exports = []
        for _ in range(self.exp_count):
            ci = c.idx(); c.idx(); c.i32(); on = c.idx(); c.u32(); sz = c.idx(); off = c.idx() if sz > 0 else 0
            self.exports.append((ci, self.names[on], sz, off))

    def cls(self, ci):
        return self.imports[-ci - 1] if ci < 0 else (self.exports[ci - 1][1] if ci > 0 else 'Class')

    def obj(self, e):
        return _x(self._rd(e[3], e[2]))


MATCLS = {'Texture', 'Shader', 'FinalBlend', 'Combiner', 'TexEnvMap', 'TexPanner'}


def cloak_materials(ukx):
    """-> {(body, id): [(class, name), ...]}  (the mesh's Materials array, in section order) and offset histogram"""
    p = Pkg(ukx)
    res = {}; offs = collections.Counter()
    for e in p.exports:
        m = re.match(r'^([MF]\w+?)_Cloak_(\d+)$', e[1])
        if not m or p.cls(e[0]) != 'SkeletalMesh':
            continue
        d = p.obj(e)
        found = None
        for off in range(40, 80):
            c = Pkg.Cur(d, off)
            try:
                k = c.idx()
                if not 1 <= k <= 4: continue
                refs = []
                for _ in range(k):
                    ci = c.idx()
                    if not (0 < ci <= len(p.exports)) or p.cls(p.exports[ci - 1][0]) not in MATCLS:
                        refs = None; break
                    refs.append((p.cls(p.exports[ci - 1][0]), p.exports[ci - 1][1]))
                if refs:
                    found = (off, refs); break
            except Exception:
                pass
        if found:
            offs[found[0]] += 1
            res[(m.group(1), int(m.group(2)))] = found[1]
    return res, offs


if __name__ == '__main__':
    res, offs = cloak_materials(sys.argv[1])
    print('meshes with materials found:', len(res), '| offset histogram:', dict(offs))
    ids = sorted({i for _, i in res})
    per_id = collections.defaultdict(set)
    for (b, i), refs in res.items():
        per_id[i].add(tuple(n for _, n in refs))
    print('cloak ids:', len(ids), '| ids whose materials differ between bodies:', [i for i in ids if len(per_id[i]) > 1])
    print('material count per mesh:', dict(collections.Counter(len(v) for v in res.values())))
    for i in ids[:4] + ids[-2:]:
        print('  id', i, sorted(per_id[i]), [res[('MFighter', i)]])
