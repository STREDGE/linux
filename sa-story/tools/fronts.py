# Куда «лицом» стоит здание на родном месте: направление на ближайшую дорогу в осях модели.
import sys, os, json, math, io, contextlib, runpy
H = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, H)
names = [a.lower() for a in sys.argv[2:]]
sys.argv = ['nodes.py', sys.argv[1], '0', '0', '1', '1']
with contextlib.redirect_stdout(io.StringIO()): g = runpy.run_path(H + '/nodes.py')
nodes = [n for n in g['nodes'].values() if n['veh']]
inst = json.load(open(H + '/all_inst.json'))
for e in inst:
    if e['name'].lower() not in names: continue
    x, y, z = e['x'], e['y'], e['z']; qx, qy, qz, qw = e['q']
    yaw = math.degrees(math.atan2(2 * (qw * qz + qx * qy), 1 - 2 * (qy * qy + qz * qz)))
    near = sorted(nodes, key=lambda n: (n['x'] - x) ** 2 + (n['y'] - y) ** 2)[:1][0]
    dx, dy = near['x'] - x, near['y'] - y; d = math.hypot(dx, dy)
    for sign in (1, -1):   # знак поворота в файле карты может быть обратным — печатаем оба
        r = math.radians(sign * yaw); lx = dx * math.cos(r) + dy * math.sin(r); ly = -dx * math.sin(r) + dy * math.cos(r)
        print('%-20s (%.0f, %.0f, %.1f) yaw %+4.0f*%+d  дорога в %3.0f м, в осях модели: x %+5.1f y %+5.1f' % (e['name'], x, y, z, yaw, sign, d, lx, ly))
