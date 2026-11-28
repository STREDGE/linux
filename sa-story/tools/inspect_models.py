# Осмотр моделей: границы, ровные площадки (крыши), вид сверху и сбоку. Запуск: python -I inspect_models.py out.png имя1 имя2 ...
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import render as R
from PIL import Image
cj = json.load(open(os.path.dirname(os.path.abspath(__file__)) + '/cols.json'))
def info(nm):
    t = R.tris_of(nm); b = cj[nm]
    n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]); ln = np.linalg.norm(n, axis=1) + 1e-9; area = ln / 2
    up = np.abs(n[:, 2]) / ln > 0.98
    zs = {}
    for i in np.where(up)[0]:
        z = round(float(t[i, :, 2].mean()) * 2) / 2; zs[z] = zs.get(z, 0) + area[i]
    top = sorted(zs.items(), key=lambda kv: -kv[1])[:5]
    print(nm, 'x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f' % (b[0], b[3], b[1], b[4], b[2], b[5]), '| ровное (высота: м2):', ', '.join('%.1f: %.0f' % (z, a) for z, a in top))
    return t
ims = []
for nm in sys.argv[2:]:
    t = info(nm)
    ax = np.array([[[0, 0, 40], [8, 0, 40], [0, 1.2, 40]]], float); ay = np.array([[[0, 0, 40], [0, 8, 40], [1.2, 0, 40]]], float)
    ims.append(R.draw([(t, (150, 165, 185)), (ax, (220, 40, 40)), (ay, (40, 170, 60))], size=(560, 460), az=0, top=True))
    ims.append(R.draw([(t, (150, 165, 185)), (ax, (220, 40, 40)), (ay, (40, 170, 60))], size=(560, 460), az=200, el=25))
W = Image.new('RGB', (560 * 4, 460 * ((len(ims) + 3) // 4)), (255, 255, 255))
for i, im in enumerate(ims): W.paste(im, ((i % 4) * 560, (i // 4) * 460))
W.save(sys.argv[1])
