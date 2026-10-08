#!/usr/bin/env python3
"""Add the animated cloaks to a client itemname-e.dat (Lineage2Ver413, Interlude layout).

  python build_cloak_itemname.py --src ItemName-e.dat --out new.dat [--first 9400] [--count 67] [--names names.csv]
                                 [--description "text"] [--template 2490]

Names default to "Animated Cloak NN" (same as tools/gen_items.py); names.csv = lines "id,Display name".
"""
import argparse, copy, csv, struct, sys, zlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2dat, itemname as I

ap = argparse.ArgumentParser()
ap.add_argument('--src', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--first', type=int, default=9400)
ap.add_argument('--count', type=int, default=67)
ap.add_argument('--template', type=int, default=2490)
ap.add_argument('--names')
ap.add_argument('--catalog', default=None, help='cloak_catalog.csv (ids and names)')
ap.add_argument('--description', default='An animated cloak.')
a = ap.parse_args()

names = {}
if a.names:
    with open(a.names, encoding='utf-8', newline='') as f:
        for row in csv.reader(f):
            if len(row) >= 2 and row[0].strip().isdigit():
                names[int(row[0])] = row[1].strip()

raw, items, end = I.load(a.src)
tail = raw[end:]
if I.serialize(items) != raw[:end]:
    sys.exit('refusing: parser does not round-trip this file')
by = {x['id']: x for x in items}
tpl = by.get(a.template)
if not tpl:
    sys.exit('template %d not found' % a.template)

ids = list(range(a.first, a.first + a.count))
if a.catalog:
    import csv
    with open(a.catalog, encoding='utf-8-sig', newline='') as fh:
        rows = list(csv.DictReader(fh))
    ids = [int(r['id']) for r in rows]; names = {int(r['id']): r['name'] for r in rows}
new = []
for n, i in enumerate(ids):
    it = copy.deepcopy(tpl)
    it['id'] = i
    it['name'] = names.get(i, 'Animated Cloak %02d' % (n + 1))
    it['description'] = ('a', a.description)
    new.append(it)

drop = set(ids) | (set(range(a.first, a.first + a.count)) if a.catalog else set())
out_items = sorted([x for x in items if x['id'] not in drop] + new, key=lambda x: x['id'])
print('source %d items; adding %d (%d..%d), replacing %d; total %d' % (len(items), len(new), ids[0], ids[-1], sum(i in by for i in ids), len(out_items)))
table = I.serialize(out_items) + tail
data = l2dat.build_file(struct.pack('<I', len(table)) + zlib.compress(table, 6))
open(a.out, 'wb').write(data)
print('wrote %s (%d bytes)' % (a.out, len(data)))

back = l2dat.decode_payload(open(a.out, 'rb').read(), strict=True)
bi, bend = I.parse(back)
assert back == table and bend == len(table) - len(tail) and back[bend:] == tail
assert [x['id'] for x in bi] == [x['id'] for x in out_items]
same = all(I.serialize([by[x['id']]]) == I.serialize([x]) for x in bi if x['id'] in by and x['id'] not in drop)
print('verify: strict decode OK, %d entries, untouched entries identical: %s' % (len(bi), same))
print('sample:', next(x for x in bi if x['id'] == ids[0]))
