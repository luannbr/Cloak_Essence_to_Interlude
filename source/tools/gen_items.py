#!/usr/bin/env python3
"""Generate the Lucera item XML for the animated cloaks (slot UNDERWEAR, like every Interlude cloak).

  python gen_items.py <out.xml> [--first 9400] [--count 67] [--names names.csv]

names.csv (optional, UTF-8): lines "id,Display name".  Without it items are called "Animated Cloak NN".
Writes UTF-8 without BOM and LF line endings, same style as data/items/*.xml.
"""
import argparse, csv, sys
from xml.sax.saxutils import quoteattr

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--first', type=int, default=9400)
ap.add_argument('--count', type=int, default=67)
ap.add_argument('--names')
ap.add_argument('--icon', default='icon.armor_back04')
ap.add_argument('--slot', default='BACK', help='Lucera equip slot: BACK (paperdoll 13, the client cloak slot) or UNDERWEAR')
ap.add_argument('--catalog', default=None, help='cloak_catalog.csv: ids, names and icons (<icon-package>.<icon_name>)')
ap.add_argument('--icon-package', default='cloakicons')
a = ap.parse_args()

names = {}
if a.names:
    with open(a.names, encoding='utf-8', newline='') as f:
        for row in csv.reader(f):
            if len(row) >= 2 and row[0].strip().isdigit():
                names[int(row[0])] = row[1].strip()

rows = []
if a.catalog:
    with open(a.catalog, encoding='utf-8-sig', newline='') as fh:
        rows = list(csv.DictReader(fh))
    a.count = len(rows)
out = ['<?xml version="1.0" encoding="UTF-8"?>', '<!DOCTYPE list SYSTEM "item.dtd">', '', '<list>']
for i in range(a.count):
    iid = int(rows[i]['id']) if rows else a.first + i
    nm = rows[i]['name'] if rows else names.get(iid, 'Animated Cloak %02d' % (i + 1))
    icon = '%s.%s' % (a.icon_package, rows[i]['icon_name']) if rows else a.icon
    out += [
        '    <armor id="%d" name=%s>' % (iid, quoteattr(nm)),
        '        <!-- [cloak_%d] -->' % iid,
        '        <set name="crystal_type" value="NONE"/>',
        '        <set name="icon" value="%s"/>' % icon,
        '        <set name="price" value="10000"/>',
        '        <set name="type" value="NONE"/>',
        '        <set name="weight" value="100"/>',
        '        <equip>',
        '            <slot id="%s"/>' % a.slot,
        '        </equip>',
        '    </armor>',
    ]
out.append('</list>')
with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(out) + '\n')
print('wrote %d armor items (%d..%d) -> %s' % (a.count, a.first, a.first + a.count - 1, a.out))
