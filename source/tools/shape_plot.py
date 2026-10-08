#!/usr/bin/env python3
"""Plots the cloth (rest pose and, optionally, a simulated frame) and the collar of tests/shape_dump.exe from the back and from the side.

  python shape_plot.py <shape.txt> <out.png> [idle_sim.txt]      (idle_sim.txt = output of cloth_real: the last frame is drawn)
"""
import sys
from PIL import Image, ImageDraw


def read(path):
    L = open(path).read().split('\n')
    i = 0
    out = {}
    while i < len(L):
        t = L[i].split()
        if t and t[0] in ('CLOTH', 'COLLAR'):
            n = int(t[1]); nt = int(t[-1]) if t[0] == 'COLLAR' else int(t[3])
            P = [tuple(float(x) for x in L[i + 1 + k].split()) for k in range(n)]
            T = [tuple(int(x) for x in L[i + 1 + n + k].split()) for k in range(nt)]
            out[t[0]] = (P, T)
            i += 1 + n + nt
        else:
            i += 1
    return out


def last_frame(path, n):
    L = open(path).read().split('\n')
    idx = [i for i, l in enumerate(L) if l.startswith('frame')]
    return [tuple(float(x) for x in L[idx[-1] + 1 + k].split()) for k in range(n)]


def draw(d, ox, oy, P, T, axes, col, scale=11.0, fill=None):
    (a, sa), (b, sb) = axes
    pt = [(ox + sa * p[a] * scale, oy - p[b] * scale) for p in P]
    for t in T:
        d.polygon([pt[t[0]], pt[t[1]], pt[t[2]]], outline=col, fill=fill)


def main():
    shape = read(sys.argv[1])
    P, T = shape['CLOTH']
    img = Image.new('RGB', (900, 560), (16, 18, 24))
    d = ImageDraw.Draw(img)
    views = [('back (x left->right mirrored)', 230, ((0, -1), (2, 1))), ('side (y front = right)', 680, ((1, 1), (2, 1)))]
    for title, ox, axes in views:
        oy = 470
        d.text((ox - 120, 8), title, fill=(200, 200, 200))
        if 'COLLAR' in shape:
            draw(d, ox, oy, *shape['COLLAR'], axes, (90, 140, 255))
        draw(d, ox, oy, P, T, axes, (90, 200, 120))
        if len(sys.argv) > 3:
            S = last_frame(sys.argv[3], len(P))
            draw(d, ox, oy, S, T, axes, (255, 160, 60))
        # torso reference: Spine2 at (0,0,32.4)
        a, b = axes[0], axes[1]
        d.line([(ox - 60, oy - 32.4 * 11), (ox + 60, oy - 32.4 * 11)], fill=(120, 120, 120))
        d.line([(ox, oy - 45 * 11), (ox, oy)], fill=(120, 120, 120))
    img.save(sys.argv[2])
    print('saved', sys.argv[2])


main()
