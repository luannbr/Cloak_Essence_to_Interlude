#!/usr/bin/env python3
"""Add the animated cloaks to a client armorgrp.dat (Lineage2Ver413, Interlude layout).

  python build_cloak_armorgrp.py --src armorgrp.dat --ukx LineShieldCloaks.ukx --out new_armorgrp.dat
         [--first 9400] [--count 67] [--template 2490] [--package LineShieldCloaks]

For every id the item is a clone of a stock cloak (--template, body_part 13) where, for each of the 14 bodies:
    mesh = <package>.<Body>_Cloak_<id>
    tex  = <package>.<material> for every entry of that mesh's own Materials[]   (feeds the pawn's CloakSkins)
Existing entries with the same ids are replaced; everything else is kept byte-for-byte.
"""
import argparse, copy, struct, sys, zlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2dat, armorgrp as A
from ukx_materials import cloak_materials

ap = argparse.ArgumentParser()
ap.add_argument('--src', required=True)
ap.add_argument('--ukx', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--first', type=int, default=9400)
ap.add_argument('--count', type=int, default=67)
ap.add_argument('--template', type=int, default=2490)
ap.add_argument('--package', default='LineShieldCloaks')
ap.add_argument('--icon', default=None, help='icon texture for the item (default: keep the template icon)')
ap.add_argument('--catalog', default=None, help='cloak_catalog.csv: the ids to add and each item\'s icon (<icon-package>.<icon_name>)')
ap.add_argument('--icon-package', default='cloakicons')
a = ap.parse_args()

raw, items, end = A.load(a.src)
tail = raw[end:]
print('source: %d items, table %d bytes, tail %r' % (len(items), end, tail))
if A.serialize(items) != raw[:end]:
    sys.exit('refusing to continue: parser does not round-trip this file')

by_id = {it['object_id']: it for it in items}
tpl = by_id.get(a.template)
if not tpl or tpl['body_part'] != 13:
    sys.exit('template %d missing or not a cloak (body_part 13)' % a.template)

mats, _ = cloak_materials(a.ukx)
ids = list(range(a.first, a.first + a.count))
icon_of = {}
if a.catalog:
    import csv
    with open(a.catalog, encoding='utf-8-sig', newline='') as fh:
        rows = list(csv.DictReader(fh))
    ids = [int(r['id']) for r in rows]
    icon_of = {int(r['id']): '%s.%s' % (a.icon_package, r['icon_name']) for r in rows}
missing = [(b, i) for i in ids for b in A.BODY_TO_KEY if (b, i) not in mats]
if missing:
    sys.exit('no mesh/materials in the ukx for: %s' % missing[:5])

new_items = []
for i in ids:
    it = copy.deepcopy(tpl)
    it['object_id'] = i
    it['weight'] = 100
    it['crystallizable'] = 0
    it['UNK_1'] = 0
    it['crystal_type'] = 0
    it['pdef'] = 0
    if a.icon:
        it['icon'][0] = a.icon
    if i in icon_of:
        it['icon'][0] = icon_of[i]
    for body, key in A.BODY_TO_KEY.items():
        it[key] = {'mesh': ['%s.%s_Cloak_%d' % (a.package, body, i)],
                   'tex': ['%s.%s' % (a.package, n) for _, n in mats[(body, i)]]}
    it['AAC'] = copy.deepcopy(it['m_HumnFigh'])
    new_items.append(it)

replaced = sum(1 for i in ids if i in by_id)
drop = set(ids) | (set(range(a.first, a.first + a.count)) if a.catalog else set())
kept = [it for it in items if it['object_id'] not in drop]
out_items = sorted(kept + new_items, key=lambda x: x['object_id'])
print('adding %d items (%d..%d), replacing %d existing, total now %d' % (len(new_items), ids[0], ids[-1], replaced, len(out_items)))

table = A.serialize(out_items) + tail
comp = struct.pack('<I', len(table)) + zlib.compress(table, 6)
data = l2dat.build_file(comp)
open(a.out, 'wb').write(data)
print('wrote %s (%d bytes, table %d bytes)' % (a.out, len(data), len(table)))

# ---- verification: decode what we wrote, parse, compare
back = l2dat.decode_payload(open(a.out, 'rb').read(), strict=True)
assert back == table, 'decoded payload differs'
bi, bend = A.parse(back)
assert bend == len(table) - len(tail) and back[bend:] == tail
assert [x['object_id'] for x in bi] == [x['object_id'] for x in out_items]
orig = {x['object_id']: x for x in items}
untouched = all(A.serialize([orig[x['object_id']]]) == A.serialize([x]) for x in bi if x['object_id'] in orig and x['object_id'] not in set(ids))
print('verify: strict decode OK, %d items parsed, untouched items identical: %s' % (len(bi), untouched))
s = next(x for x in bi if x['object_id'] == ids[0])
print('sample %d: body_part %d, m_Elf = %s' % (s['object_id'], s['body_part'], s['m_Elf']))
