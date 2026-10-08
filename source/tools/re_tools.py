"""Small reverse-engineering helpers for the L2 engine DLLs (read-only).

  python re_tools.py exp  <dll> <substring> [...]      list exports whose name contains any substring (real address after jmp thunks)
  python re_tools.py dis  <dll> <va_hex> <count>        disassemble `count` instructions from a VA (annotates strings)
  python re_tools.py calls <dll> <substring> [...]      direct call sites (E8) of the matching exports, with the owning export
"""
import sys, struct, bisect, re
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[0])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.d = open(path, 'rb').read()

    def off(self, va):
        rva = va - self.base
        for s in self.pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                return s.PointerToRawData + rva - s.VirtualAddress
        return None

    def follow(self, va):
        for _ in range(4):
            o = self.off(va)
            if o is not None and o < len(self.d) and self.d[o] == 0xE9:
                va = va + 5 + struct.unpack_from('<i', self.d, o + 1)[0]
            else:
                break
        return va

    def exports(self):
        return [(self.follow(self.base + e.address), e.name.decode(), self.base + e.address)
                for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name]

    def cstr(self, va):
        o = self.off(va)
        if o is None: return None
        b = self.d[o:o + 100]; a = b.split(b'\0')[0]
        if len(a) >= 3 and all(32 <= c < 127 for c in a): return 'A"%s"' % a.decode()
        w = b''
        for k in range(0, 98, 2):
            if b[k + 1:k + 2] == b'\0' and 32 <= b[k] < 127: w += b[k:k + 1]
            else: break
        return 'W"%s"' % w.decode() if len(w) >= 3 else None


def main():
    cmd, dll = sys.argv[1], sys.argv[2]
    im = Img(dll)
    if cmd == 'exp':
        subs = sys.argv[3:]
        for real, name, thunk in sorted(im.exports()):
            if any(s in name for s in subs):
                print('real 0x%x (rva 0x%x)  %s' % (real, real - im.base, name[:200]))
    elif cmd == 'dis':
        va = int(sys.argv[3], 16); n = int(sys.argv[4])
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        o = im.off(va)
        for k, ins in enumerate(md.disasm(im.d[o:o + n * 8], va)):
            ex = ''
            for m in re.finditer(r'0x1[0-9a-f]{7}', ins.op_str):
                s = im.cstr(int(m.group(), 16))
                if s: ex = '   ; ' + s
            print('%08x  %-7s %s%s' % (ins.address, ins.mnemonic, ins.op_str, ex))
            if k + 1 >= n: break
    elif cmd == 'disx':                                                # like dis, but names call targets that are engine exports
        va = int(sys.argv[3], 16); n = int(sys.argv[4])
        byaddr = {}
        for real, name, thunk in im.exports():
            byaddr.setdefault(real, name); byaddr.setdefault(thunk, name)
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        o = im.off(va)
        for k, ins in enumerate(md.disasm(im.d[o:o + n * 8], va)):
            ex = ''
            m = re.match(r'0x([0-9a-f]+)$', ins.op_str)
            if ins.mnemonic in ('call', 'jmp') and m:
                nm = byaddr.get(int(m.group(1), 16))
                if nm: ex = '   ; ' + nm[:110]
            for mm in re.finditer(r'0x1[0-9a-f]{7}', ins.op_str):
                s = im.cstr(int(mm.group(), 16))
                if s: ex = '   ; ' + s
            print('%08x  %-7s %s%s' % (ins.address, ins.mnemonic, ins.op_str, ex))
            if k + 1 >= n: break
    elif cmd == 'calls':
        subs = sys.argv[3:]
        ex = sorted(im.exports()); keys = [e[0] for e in ex]
        targets = {}
        for real, name, thunk in ex:
            if any(s in name for s in subs):
                targets[real] = name; targets[thunk] = name
        execs = [s for s in im.pe.sections if s.Characteristics & 0x20000000]
        sec = max(execs, key=lambda s: s.SizeOfRawData)
        t0 = sec.PointerToRawData; t1 = t0 + sec.SizeOfRawData; tva = im.base + sec.VirtualAddress
        res = {}
        i = t0
        while True:
            i = im.d.find(b'\xE8', i, t1 - 5)
            if i < 0: break
            va = tva + i - t0
            dst = va + 5 + struct.unpack_from('<i', im.d, i + 1)[0]
            if dst in targets: res.setdefault(targets[dst], []).append(va)
            i += 1
        for name in sorted({targets[k] for k in targets}):
            lst = res.get(name, [])
            print('%s : %d call sites' % (name[:110], len(lst)))
            for va in lst[:8]:
                j = bisect.bisect_right(keys, va) - 1
                print('     0x%x in %s +0x%x' % (va, ex[j][1][:70] if j >= 0 else '?', va - ex[j][0] if j >= 0 else 0))


main()
