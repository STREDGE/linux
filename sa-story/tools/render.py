# Рисовальщик формы объектов игры по их коллизии (треугольники + коробки) — чтобы видеть, что ставим.
# Без сторонних библиотек, кроме numpy и PIL. Вид — сверху-сбоку (изометрия), закраска по наклону грани.
import pickle, json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
H = os.path.dirname(os.path.abspath(__file__))
_meshes = None
def meshes():
    global _meshes
    if _meshes is None: _meshes = pickle.load(open(H + '/meshes.pkl', 'rb'))
    return _meshes
def tris_of(name):
    """Треугольники модели в её собственных координатах: массив (n, 3, 3)."""
    m = meshes().get(name.lower())
    if not m: return np.zeros((0, 3, 3))
    verts, faces, boxes = m
    out = []
    v = np.array(verts, dtype=float) if verts else np.zeros((0, 3))
    for f in faces:
        if max(f[:3]) < len(v): out.append([v[f[0]], v[f[1]], v[f[2]]])
    for b in boxes:
        x0, y0, z0, x1, y1, z1 = b
        c = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
        for q in ((0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            out.append([c[q[0]], c[q[1]], c[q[2]]]); out.append([c[q[0]], c[q[2]], c[q[3]]])
    return np.array(out, dtype=float) if out else np.zeros((0, 3, 3))
def place(tris, x, y, z, heading_deg):
    r = math.radians(heading_deg); c, s = math.cos(r), math.sin(r)
    R = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
    return tris @ R.T + np.array([x, y, z])
def draw(items, size=(1400, 1000), az=35, el=35, bg=(232, 236, 240), labels=None, zoom=None, center=None, top=False, base=0):
    """items: список (треугольники (n,3,3), цвет (r,g,b)). Возвращает картинку."""
    allt = [t for t, _ in items if len(t)]
    if not allt: return Image.new('RGB', size, bg)
    P = np.concatenate(allt)
    a, e = math.radians(az), math.radians(90 if top else el)
    # оси экрана: вправо, вверх, вглубь
    right = np.array([math.cos(a), math.sin(a), 0])
    fwd = np.array([-math.sin(a) * math.cos(e), math.cos(a) * math.cos(e), -math.sin(e)])
    up = np.cross(right, fwd)
    pts = P.reshape(-1, 3)
    sx, sy = pts @ right, pts @ up
    if center is None: cx, cy = (sx.min() + sx.max()) / 2, (sy.min() + sy.max()) / 2
    else: cx, cy = np.array(center) @ right, np.array(center) @ up
    if zoom is None: zoom = 0.92 * min(size[0] / max(1e-3, sx.max() - sx.min()), size[1] / max(1e-3, sy.max() - sy.min()))
    img = Image.new('RGB', size, bg); d = ImageDraw.Draw(img)
    light = np.array([0.35, -0.45, 0.82]); light = light / np.linalg.norm(light)
    polys = []
    for idx, (tris, col) in enumerate(items):
        if not len(tris): continue
        n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        ln = np.linalg.norm(n, axis=1); ln[ln == 0] = 1
        sh = 0.45 + 0.55 * np.abs((n / ln[:, None]) @ light)
        depth = (tris @ fwd).mean(axis=1)
        X = (tris @ right - cx) * zoom + size[0] / 2
        Y = size[1] / 2 - (tris @ up - cy) * zoom
        for i in range(len(tris)):
            polys.append((depth[i] + (1e6 if idx < base else 0), [(X[i, 0], Y[i, 0]), (X[i, 1], Y[i, 1]), (X[i, 2], Y[i, 2])], tuple(int(c * sh[i]) for c in col)))
    polys.sort(key=lambda p: -p[0])
    for _, pg, col in polys: d.polygon(pg, fill=col)
    if labels:
        try: font = ImageFont.truetype('arial.ttf', 15)
        except Exception: font = ImageFont.load_default()
        for (x, y, z, text) in labels:
            p = np.array([x, y, z]); px = (p @ right - cx) * zoom + size[0] / 2; py = size[1] / 2 - (p @ up - cy) * zoom
            w = d.textlength(text, font=font)
            d.rectangle([px - w / 2 - 3, py - 10, px + w / 2 + 3, py + 9], fill=(255, 255, 255))
            d.text((px - w / 2, py - 9), text, fill=(20, 20, 20), font=font)
    return img
def sheet(names, out, cols=4, cell=(420, 330)):
    """Лист превью: каждая модель в своей клетке, с именем и размером."""
    cj = json.load(open(H + '/cols.json'))
    rows = (len(names) + cols - 1) // cols
    img = Image.new('RGB', (cols * cell[0], rows * cell[1]), (255, 255, 255)); d = ImageDraw.Draw(img)
    try: font = ImageFont.truetype('arial.ttf', 15)
    except Exception: font = ImageFont.load_default()
    for i, nm in enumerate(names):
        t = tris_of(nm)
        im = draw([(t, (150, 165, 185))], size=(cell[0], cell[1] - 26), az=35, el=30, bg=(244, 246, 248))
        x, y = (i % cols) * cell[0], (i // cols) * cell[1]
        img.paste(im, (x, y + 26))
        b = cj.get(nm.lower())
        cap = nm + ('  %.0fx%.0fx%.0f, tri %d' % (b[3] - b[0], b[4] - b[1], b[5] - b[2], len(t)) if b else '  (нет)')
        d.text((x + 4, y + 4), cap, fill=(0, 0, 0), font=font)
    img.save(out); return out
if __name__ == '__main__':
    sheet(sys.argv[2:], sys.argv[1])
