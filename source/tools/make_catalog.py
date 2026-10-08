#!/usr/bin/env python3
"""Write data/cloak_catalog.csv: ids 9400-9409 = the animated mantles (existing rows kept) + ids 9410.. = the cloth looks of essence_cloth.bin
with the official item name and icon of the first Essence item that uses the look.

  python make_catalog.py <essence_cloth.bin> [--out cloak_catalog.csv]
"""
import os, sys, csv, struct, argparse
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import essence_cloaks as E

DATA = os.environ.get('CLOAK_DATA', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data')) + os.sep


def read_looks(path):
    d = open(path, 'rb').read()
    p = 4
    nt, ns, nl = struct.unpack_from('<III', d, p); p += 12
    def cstr():
        nonlocal p
        e = d.index(b'\0', p); s = d[p:e].decode('latin1'); p = e + 1; return s
    for _ in range(nt):
        cstr(); fmt, w, h, lv = struct.unpack_from('<IIII', d, p); p += 16
        for _ in range(lv):
            sz = struct.unpack_from('<I', d, p)[0]; p += 4 + sz
    for _ in range(ns):
        cstr(); cstr(); np_, wd, nt_, na, nsp, nc = struct.unpack_from('<IIIIII', d, p); p += 24
        p += np_ * 12 + np_ * 8 + np_ * 4 + nt_ * 6 + na * 2 + nsp * 8
        for _ in range(nc): cstr(); cstr(); p += 4
    looks = []
    for _ in range(nl):
        name = cstr(); kind = chr(d[p]); p += 1; tex, item = struct.unpack_from('<II', d, p); p += 8
        looks.append((name, kind, tex, item))
    return looks


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('pack'); ap.add_argument('--out', default=DATA + 'cloak_catalog.csv')
    a = ap.parse_args()
    items = {int(f['object_id']): f for f in E.parse_armorgrp(DATA + 'essence_dat\\Armorgrp.txt')}
    looks = read_looks(a.pack)
    # keep the first 10 rows (animated mantles) from the current catalog
    with open(a.out, encoding='utf-8-sig', newline='') as fh:
        rows = [r for r in csv.DictReader(fh) if int(r['id']) < 9410]
    have = set(os.path.splitext(f)[0] for f in os.listdir(DATA + 'icons_essence_all'))
    for k, (name, kind, tex, item) in enumerate(looks):
        ref = (E.BR.findall(items[item].get('icon', '')) or ['None'])[0]
        stem = ref.replace('.', '__')
        if stem not in have:
            alt = ref.split('.')[-1]
            stem = next((h for h in have if h.endswith('__' + alt) or h == alt), 'icon__vesper_cloack_i00')
        rows.append(dict(id=9410 + k, design='cloth', name=name, icon_src='ALL:' + stem, icon_name='cloak_%02d' % (10 + k)))
    with open(a.out, 'w', encoding='utf-8', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=['id', 'design', 'name', 'icon_src', 'icon_name']); w.writeheader(); w.writerows(rows)
    print('wrote', a.out, len(rows), 'rows (%d cloth looks)' % len(looks))
    missing = [r for r in rows if r['icon_src'].startswith('ALL:') and r['icon_src'][4:] not in have]
    print('icons missing:', [r['icon_src'] for r in missing])
