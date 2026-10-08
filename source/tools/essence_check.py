"""Independent reference for tests/essence_test.cpp: skin a mantle from the ORIGINAL ukx and print the same numbers.
   python essence_check.py <mantles.ukx> <Body> <SeqName> <frame>"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence import Package, parse_mesh, parse_anim
from essence_skin import global_transforms, bind_locals
from essence_preview import anim_pose, skin
from build_capes import find_ci

pk = Package(sys.argv[1]); body, seq, frame = sys.argv[2], sys.argv[3], float(sys.argv[4])
mesh = parse_mesh(pk, find_ci(pk, 'SkeletalMesh', '%s_NewMantle00_m_ad00' % body))
anim = parse_anim(pk, find_ci(pk, 'MeshAnimation', body + '_cape_anim'))
si = next(i for i, s in enumerate(anim['seqs']) if s['name'].lower() == seq.lower())
bind_G = global_transforms([b[1] for b in mesh['bones']], bind_locals(mesh['bones'], False, True))
pts = skin(mesh, bind_G, anim_pose(anim, si, frame))
print('sum %.4f %.4f %.4f' % tuple(pts.sum(axis=0)))
for i in (0, 1, 100, 1000, 2000, 2923):
    print('p%d %.4f %.4f %.4f' % (i, *pts[i]))
