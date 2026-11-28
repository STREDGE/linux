# Четыре вида одной модели крупно, грани раскрашены по высоте. Запуск: python -I view4.py out.png имя
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, render as R
from PIL import Image
nm = sys.argv[2]; t = R.tris_of(nm)
zc = t[:, :, 2].mean(axis=1); z0, z1 = zc.min(), zc.max()
items = []
for lo in np.linspace(z0, z1, 9)[:-1]:
    hi = lo + (z1 - z0) / 8; m = (zc >= lo) & (zc <= hi + 1e-6); k = (lo - z0) / (z1 - z0 + 1e-9)
    items.append((t[m], (int(90 + 150 * k), int(150 - 40 * k), int(210 - 150 * k))))
ims = [R.draw(items, size=(800, 600), az=0, top=True)] + [R.draw(items, size=(800, 600), az=a, el=28) for a in (20, 160, 250)]
W = Image.new('RGB', (1600, 1200), (255, 255, 255))
for i, im in enumerate(ims): W.paste(im, ((i % 2) * 800, (i // 2) * 600))
W.save(sys.argv[1])
