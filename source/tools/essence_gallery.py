"""Gallery of every mantle of one body in bind pose (back view, textured).
   python essence_gallery.py <mantles.ukx> <tex.utx> <BodyPrefix> <out.png> [cols=5]"""
import sys, os, math
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from essence import Package, parse_mesh
from essence_dxt import TexPackage
from essence_preview2 import rasterise, mesh_materials


def main():
    pk = Package(sys.argv[1]); tp = TexPackage(sys.argv[2])
    prefix = sys.argv[3].lower(); out = sys.argv[4]; cols = int(sys.argv[5]) if len(sys.argv) > 5 else 5
    tiles = []
    for e in pk.exports:
        if pk.cls_name(e['cls']) != 'SkeletalMesh' or not e['name'].lower().startswith(prefix + '_'):
            continue
        try:
            mesh = parse_mesh(pk, e)
            mats = mesh_materials(pk, mesh, tp)
            im = rasterise(np.array(mesh['points']), mesh, mats, size=(300, 360), view='back', zr=(-12, 62), hr=(-36, 36))
            label = '%s%s' % (e['name'].replace('_m_ad00', ''), '' if mesh.get('sections') or len(mats) == 1 else '  (sections?)')
        except Exception as ex:
            im = Image.new('RGB', (300, 360), (40, 20, 20)); label = '%s  ERR %s' % (e['name'], str(ex)[:40])
        ImageDraw.Draw(im).text((6, 4), label, fill=(230, 230, 230))
        tiles.append(im)
        print('rendered', label)
    w, h = tiles[0].size
    rows = math.ceil(len(tiles) / cols)
    sheet = Image.new('RGB', (cols * w, rows * h), (20, 20, 24))
    for i, im in enumerate(tiles):
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    sheet.save(out); print('saved', out, sheet.size)


if __name__ == '__main__':
    main()
