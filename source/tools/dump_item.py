"""python dump_item.py <armorgrp.dat> <object_id> [<object_id> ...]   (prints every non-empty field)"""
import sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, __import__('os').path.dirname(__file__))
import armorgrp as A

raw, items, end = A.load(sys.argv[1])
print('items', len(items), '| table end', end, '| tail bytes', raw[end:].hex())
by = {it['object_id']: it for it in items}
for oid in map(int, sys.argv[2:]):
    it = by.get(oid)
    if not it:
        print('== %d not found' % oid); continue
    print('== object_id %d' % oid)
    for k, v in it.items():
        if isinstance(v, dict):
            if v['mesh'] != [''] or v['tex'] != ['']:
                if v['mesh'] or v['tex']:
                    print('  %-15s mesh=%s tex=%s' % (k, v['mesh'], v['tex']))
        elif isinstance(v, list):
            if any(v): print('  %-15s %s' % (k, v))
        else:
            print('  %-15s %s' % (k, v))
