#!/usr/bin/env python3
"""List every Essence cloak item (body_part=back) with its meshes/textures per race and its name, from the unpacked Armorgrp.txt + ItemName-eu.txt.

  python essence_cloaks.py [--armorgrp Armorgrp.txt] [--itemname ItemName-eu.txt] [--out cloaks_essence.csv]

The two .txt files come from the DatEditor CLI (see essence_dat.py / UGX_System\\dateditor-cli.jar):
  java -cp "lib/*;..\\dateditor-cli.jar" l2s.dateditor.cli.DatEditorCli unpack <Armorgrp.dat> --out Armorgrp.txt
"""
import re, sys, csv, argparse, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

RACES = [('m_HumnFigh', 'MFighter'), ('f_HumnFigh', 'FFighter'), ('m_HumnMyst', 'MMagic'), ('f_HumnMyst', 'FMagic'), ('m_Elf', 'MElf'), ('f_Elf', 'FElf'),
         ('m_DarkElf', 'MDarkElf'), ('f_DarkElf', 'FDarkElf'), ('m_Dorf', 'MDwarf'), ('f_Dorf', 'FDwarf'), ('m_OrcFigh', 'MOrc'), ('f_OrcFigh', 'FOrc'),
         ('m_OrcMage', 'MShaman'), ('f_OrcMage', 'FShaman')]
BR = re.compile(r'\[([^\]]*)\]')


def parse_armorgrp(path):
    items = []
    for line in open(path, encoding='utf-8', errors='replace'):
        if not line.startswith('item_begin'):
            continue
        f = {}
        for part in line.rstrip('\n').split('\t'):
            k, _, v = part.partition('=')
            f[k] = v
        items.append(f)
    return items


def parse_names(path):
    names = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'(?:^|\t)id=(\d+)', line) or re.search(r'object_id=(\d+)', line)
        if not m:
            continue
        n = re.search(r'(?:^|\t)name=\[([^\]]*)\]', line) or re.search(r'(?:^|\t)name=([^\t]*)', line)
        if n:
            names[int(m.group(1))] = n.group(1)
    return names


def cloaks(items):
    out = []
    for f in items:
        if f.get('body_part') != 'back':
            continue
        per = {}
        for key, body in RACES:
            v = f.get(key + '_add', '')
            br = BR.findall(v)
            br = [b for b in br if b != 'None']
            if br:
                per[body] = br                                   # first = mesh, the rest = textures
        icon = (BR.findall(f.get('icon', '')) or ['None'])[0]
        out.append(dict(id=int(f['object_id']), icon=icon, per=per, rec=f))
    return out


if __name__ == '__main__':
    d = os.path.join(os.environ.get('CLOAK_DATA', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data')), 'essence_dat') + os.sep
    ap = argparse.ArgumentParser()
    ap.add_argument('--armorgrp', default=d + 'Armorgrp.txt'); ap.add_argument('--itemname', default=d + 'ItemName-eu.txt'); ap.add_argument('--out', default=d + 'cloaks_essence.csv')
    a = ap.parse_args()
    items = parse_armorgrp(a.armorgrp); names = parse_names(a.itemname)
    cl = cloaks(items)
    print('%d armorgrp records, %d with body_part=back, %d names loaded' % (len(items), len(cl), len(names)))
    with open(a.out, 'w', encoding='utf-8', newline='') as fh:
        w = csv.writer(fh); w.writerow(['id', 'name', 'icon', 'bodies_with_mesh', 'mesh_MFighter', 'tex_MFighter', 'mesh_MDarkElf', 'mesh_FElf'])
        for c in sorted(cl, key=lambda c: c['id']):
            p = c['per']
            w.writerow([c['id'], names.get(c['id'], ''), c['icon'], len(p), (p.get('MFighter') or [''])[0], '|'.join((p.get('MFighter') or [''])[1:]), (p.get('MDarkElf') or [''])[0], (p.get('FElf') or [''])[0]])
    print('wrote', a.out)
