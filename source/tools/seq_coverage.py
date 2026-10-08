"""Which body animation sequences of the Interlude client have a baked cape animation (case-insensitive name match)?
   python seq_coverage.py <Interlude Body.ukx> <mantles.ukx> <BodySuffix e.g. MDarkElf> <BodyCapeAnim e.g. MDarkElf_cape_anim>"""
import sys, os, re, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2pkg import Package
from essence import parse_anim
from build_capes import find_ci

body_pk = Package(sys.argv[1], names_only=True)
cape_pk = Package(sys.argv[2])
suffix = sys.argv[3].lower()
anim = parse_anim(cape_pk, find_ci(cape_pk, 'MeshAnimation', sys.argv[4]))
cape = {s['name'].lower() for s in anim['seqs']}
# body sequence names: names ending with _<Body> (case-insensitive) in the Interlude package
mine = sorted({n for n in body_pk.names if n.lower().endswith('_' + suffix)})
miss = [n for n in mine if n.lower() not in cape]
print('Interlude %s sequences: %d ; with a cape animation: %d ; WITHOUT: %d' % (suffix, len(mine), len(mine) - len(miss), len(miss)))
print('cape sequences not used by the Interlude body: %d' % len(cape - {n.lower() for n in mine}))
print('--- Interlude sequences with no cape animation:')
for n in miss:
    print('  ', n)
