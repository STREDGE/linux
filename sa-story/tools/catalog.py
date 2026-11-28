# Каталог объектов игры по ключевым словам: номер, имя, размер по коллизии, дальность прорисовки, где стоит на карте.
# Запуск: python -I catalog.py <gamedata> слово1 слово2 ... [--min N] [--max N]
import json, sys, glob, re, os
G = sys.argv[1]
args = [a for i, a in enumerate(sys.argv[2:]) if not a.startswith('--') and not sys.argv[1 + i].startswith('--')]
mn = float(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 0
mx = float(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 1e9
H = os.path.dirname(os.path.abspath(__file__))
cols = json.load(open(H + '/cols.json'))
ide = {}
for p in glob.glob(G + '/data/**/*.ide', recursive=True) + glob.glob(G + '/data/**/*.IDE', recursive=True):
    sec = None
    for line in open(p, errors='ignore'):
        l = line.split('#')[0].strip()
        if not l: continue
        if l in ('objs', 'tobj', 'anim'): sec = l; continue
        if l == 'end': sec = None; continue
        if sec:
            f = [x.strip() for x in l.split(',')]
            try:
                dd = float(f[-2])
                ide[int(f[0])] = (f[1], f[2], dd, os.path.basename(p))
            except Exception: pass
inst = {}
try:
    for e in json.load(open(H + '/all_inst.json')):
        inst.setdefault(e['id'], []).append((e['x'], e['y'], e['z']))
except Exception: pass
rows = []
for mid, (name, txd, dd, f) in ide.items():
    low = name.lower()
    if args and not any(re.search(a, low) for a in args): continue
    if 'lod' in low: continue
    b = cols.get(low)
    if not b: continue
    sx, sy, sz = b[3] - b[0], b[4] - b[1], b[5] - b[2]
    big = max(sx, sy)
    if big < mn or big > mx: continue
    at = inst.get(mid, [])
    where = ('%d шт., напр. (%.0f, %.0f)' % (len(at), at[0][0], at[0][1])) if at else 'на карте нет'
    rows.append((name, mid, sx, sy, sz, b[2], dd, f, where))
for r in sorted(rows):
    print('%5d %-22s %5.1f x %5.1f x %4.1f  низ %5.1f  видно %3.0f  %-14s %s' % (r[1], r[0], r[2], r[3], r[4], r[5], r[6], r[7], r[8]))
print('всего:', len(rows))
