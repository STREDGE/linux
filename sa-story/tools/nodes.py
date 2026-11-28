# Дорожные узлы игры (NODES*.DAT из gta3.img) вокруг базы: где проходят дороги для машин и пешеходов.
# Запуск: python -I nodes.py <gamedata> [x0 y0 x1 y1]
import struct, sys
G = sys.argv[1]
R = [float(a) for a in sys.argv[2:6]] if len(sys.argv) >= 6 else [2500, -2560, 2870, -2230]
f = open(G + '/texdb/gta3.img', 'rb'); h = f.read(8); n = struct.unpack('<I', h[4:8])[0]
ents = [struct.unpack('<IHH24s', f.read(32)) for _ in range(n)]
files = {}
for off, sz, sz2, nm in ents:
    name = nm.split(b'\0')[0].decode().lower()
    if name.startswith('nodes') and name.endswith('.dat'):
        f.seek(off * 2048); files[int(name[5:-4])] = f.read((sz or sz2) * 2048)
print('файлов узлов:', len(files))
nodes = {}   # (area, id) -> dict
links = {}
for area, d in files.items():
    nn, nv, np_, nnavi, nl = struct.unpack_from('<5I', d, 0)
    p = 20
    for i in range(nn):
        mem, zero, x, y, z, cost, link, ar, nid, width, flood, flags = struct.unpack_from('<IIhhhhHHHBBI', d, p + i * 28)
        nodes[(area, i)] = dict(x=x / 8.0, y=y / 8.0, z=z / 8.0, link=link, n=flags & 15, veh=i < nv, flags=flags, width=width)
    p += nn * 28 + nnavi * 14
    links[area] = [struct.unpack_from('<HH', d, p + i * 4) for i in range(nl)]
def inr(nd, pad=0): return R[0] - pad < nd['x'] < R[2] + pad and R[1] - pad < nd['y'] < R[3] + pad
veh = [(k, nd) for k, nd in nodes.items() if nd['veh'] and inr(nd)]
ped = [(k, nd) for k, nd in nodes.items() if not nd['veh'] and inr(nd)]
print('узлов машин в окне: %d, пешеходных: %d' % (len(veh), len(ped)))
print('--- машины: узел (x, y, z) соседей -> соседи ---')
for k, nd in sorted(veh, key=lambda t: (t[1]['y'], t[1]['x'])):
    nb = []
    for j in range(nd['n']):
        a, i = links[k[0]][nd['link'] + j]
        o = nodes.get((a, i))
        if o: nb.append('(%.0f,%.0f)' % (o['x'], o['y']))
    print('%d:%d (%.1f, %.1f, %.1f) n=%d -> %s' % (k[0], k[1], nd['x'], nd['y'], nd['z'], nd['n'], ' '.join(nb)))
if '--ped' in sys.argv:
    print('--- пешеходы ---')
    for k, nd in sorted(ped, key=lambda t: (t[1]['y'], t[1]['x'])):
        print('%d:%d (%.1f, %.1f, %.1f)' % (k[0], k[1], nd['x'], nd['y'], nd['z']))

# --- цепочки от базы до ближайших развилок (n>=3) ---
if '--chains' in sys.argv:
    B = [2600, -2460, 2768, -2332]
    def nbrs(k):
        nd = nodes[k]; out = []
        for j in range(nd['n']):
            a, i = links[k[0]][nd['link'] + j]
            if (a, i) in nodes: out.append((a, i))
        return out
    inside = [k for k, nd in nodes.items() if nd['veh'] and B[0] < nd['x'] < B[2] and B[1] < nd['y'] < B[3]]
    seen = set(inside); front = list(inside); ends = []
    while front:
        k = front.pop()
        for o in nbrs(k):
            if o in seen: continue
            seen.add(o)
            if nodes[o]['n'] >= 3: ends.append(o); continue
            front.append(o)
    xs = [nodes[k]['x'] for k in seen]; ys = [nodes[k]['y'] for k in seen]
    print('узлов внутри базы: %d; цепочка до развилок: %d узлов; охват x %.0f..%.0f, y %.0f..%.0f' % (len(inside), len(seen), min(xs), max(xs), min(ys), max(ys)))
    for e in ends:
        nd = nodes[e]; print('  развилка %d:%d (%.0f, %.0f, %.0f) n=%d -> %s' % (e[0], e[1], nd['x'], nd['y'], nd['z'], nd['n'], ' '.join('(%.0f,%.0f)' % (nodes[o]['x'], nodes[o]['y']) for o in nbrs(e))))
    print('  узлы цепочки вне базы:')
    for k in sorted(seen - set(inside), key=lambda k: (nodes[k]['x'], nodes[k]['y'])):
        nd = nodes[k]; print('   %d:%d (%.0f, %.0f, %.0f) n=%d' % (k[0], k[1], nd['x'], nd['y'], nd['z'], nd['n']))
