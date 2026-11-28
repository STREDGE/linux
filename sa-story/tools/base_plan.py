# План базы S.W.A.T. v2 — единый источник: из него рисуется рендер и генерируется base_plan.h для мода.
# Координаты плана: u — на запад (+), v — на юг (+), центр (2684, −2396). Мир: x = 2684 − u, y = −2396 − v.
# Запуск: python -I base_plan.py [--header путь] [--render папка]
import sys, os, json, math
H = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, H)
import numpy as np
CX, CY, KZ = 2684.0, -2396.0, 12.62
cols = json.load(open(H + '/cols.json'))
ID = {'vgnpolicebuild2': 7009, 'des_stmedicentre_': 16564, 'cunts_ammun': 18552, 'mil_hangar1_': 3268, 'a51_spottower': 3279,
      'a51_gatecontrol': 16093, 'des_a51guardbox02': 16095, 'pol_comp_gate': 2933, 'elecfence_bar': 987, 'portakabin': 1684,
      'ws_floodlight': 3864, 'dockbarr1_la': 3578, 'helipad': 3928, 'helipad01': 3934, 'kmb_container_red': 2934, 'target_frame': 3025,
      'tall_fence': 974, 'parkbench1': 1280, 'parktable1': 1281, 'grill': 914, 'satdishsml': 1596, 'ap_radar1_01': 1682,
      'cxrf_watertwr': 3286, 'cxrf_oiltank': 3287, 'generator_big': 934, 'kmilitary_crate': 2977, 'crates': 18257,
      'cj_flame_drum': 918, 'cj_dumpster': 1345, 'cj_skip_rubbish': 1358, 'sfx_flag02': 3807, 'vegasflag1': 7090, 'ct_table': 3041,
      'trafficcone': 1238, 'police_cell_bed': 2603, 'police_cell_toilet': 2602}
pieces = []   # dict(model, name, x, y, z, h, look, what, color)
def W(u, v): return CX - u, CY - v
def put(name, u, v, h=0.0, dz=0.0, look=-1, what='', color=(150, 160, 175)):
    """Поставить модель: центр её коробки — в точку плана (u, v), низ — на пол (+dz), поворот h (мировой курс, градусы)."""
    b = cols[name]; cx, cy = (b[0] + b[3]) / 2, (b[1] + b[4]) / 2
    r = math.radians(h); wx, wy = W(u, v)
    x = wx - (cx * math.cos(r) - cy * math.sin(r)); y = wy - (cx * math.sin(r) + cy * math.cos(r))
    pieces.append(dict(model=ID[name], name=name, x=x, y=y, z=KZ + dz - b[2], h=h % 360, look=look, what=what or name, color=color))
def put_origin(name, x, y, z, h=0.0, look=-1, what='', color=(150, 160, 175)):
    pieces.append(dict(model=ID[name], name=name, x=x, y=y, z=z, h=h % 360, look=look, what=what or name, color=color))
def local(p, lx, ly):
    """Точка в осях уже поставленной модели → план (u, v)."""
    r = math.radians(p['h']); x = p['x'] + lx * math.cos(r) - ly * math.sin(r); y = p['y'] + lx * math.sin(r) + ly * math.cos(r)
    return CX - x, CY - y
P = {}   # точки для кода (u, v)

# ---------------- забор: звенья 12 м, сплошной, один разрыв — ворота ----------------
F = (95, 105, 95)
for k in range(6):                                     # юг (сторона ворот): от столбов ворот к углам
    put('elecfence_bar', 15 + 12 * k, 60, 0, what='забор', color=F); put('elecfence_bar', -15 - 12 * k, 60, 0, what='забор', color=F)
for k in range(13): put('elecfence_bar', -74 + 12 * k, -60, 0, what='забор', color=F)
put('elecfence_bar', 74, -60, 0, what='забор', color=F)
for k in range(10):
    put('elecfence_bar', 80, -54 + 12 * k, 90, what='забор', color=F); put('elecfence_bar', -80, -54 + 12 * k, 90, what='забор', color=F)

# ---------------- КПП: откатные ворота (2 створки), домик охраны, диспетчерская, «змейка» из блоков ----------------
put('pol_comp_gate', 4.45, 60, 0, what='ворота (створка запад)', color=(70, 80, 110))
put('pol_comp_gate', -4.45, 60, 0, what='ворота (створка восток)', color=(70, 80, 110))
put('des_a51guardbox02', 14, 54, 0, what='домик охраны КПП', color=(170, 150, 120))
put('a51_gatecontrol', -19, 50, 180, what='диспетчерская КПП', color=(170, 150, 120))
put('dockbarr1_la', -3.6, 47, 0, what='бетонный блок (змейка)', color=(200, 190, 90))
put('dockbarr1_la', 3.6, 37, 0, what='бетонный блок (змейка)', color=(200, 190, 90))
put('dockbarr1_la', 77, 6, 90, what='блок: тупик дороги на мост', color=(200, 190, 90))
put('dockbarr1_la', -76, 57, 0, what='блок: тупик восточной петли', color=(200, 190, 90))
P['Gate'] = (0, 60)

# ---------------- штаб: здание полиции (vgnpolicebuild2), вход на юг, задний корпус — к северному забору ----------------
hq = dict(x=CX + 2.25, y=-2377.3, h=0.0)
put_origin('vgnpolicebuild2', hq['x'], hq['y'], KZ + 14.03, 0, what='штаб', color=(196, 170, 130))
ROOF = 27.96                                            # крыша башни над полом
hqp = pieces[-1]
P['Hq'] = local(hqp, 0, 0)
P['Pad'] = local(hqp, -16, 10); P['Heli2'] = local(hqp, 10, 10)
for key in ('Pad', 'Heli2'):
    u, v = P[key]; put('helipad' if key == 'Pad' else 'helipad01', u, v, 0, dz=ROOF + 0.04, what='вертолётная площадка', color=(230, 230, 120))
P['Frank'] = local(hqp, 8, -17)                         # на площади перед крыльцом штаба (крыльцо поднято на 4 м, к нему пандус)
P['Lift'] = local(hqp, -20, -4.5)                       # подъём на крышу — у левого крыла
P['RoofLift'] = local(hqp, -28, 4)
u, v = local(hqp, 24, 16); put('satdishsml', u, v, 200, dz=ROOF, what='антенна на крыше', color=(210, 210, 215))
for lx in (-24, -10):                                   # флаги на фасаде (настенная модель — на стене, как задумано)
    u, v = local(hqp, lx, -2.1); x, y = W(u, v); put_origin('vegasflag1', x, y, KZ + 10.0, -90, what='флаг на фасаде штаба', color=(200, 60, 60))

# ---------------- северо-запад: медцентр, скорая, радар, водонапорка ----------------
put('des_stmedicentre_', 57, -30, 180, what='медпункт (медцентр)', color=(215, 215, 225))
P['Med'] = (57, -10)                                    # метка перед фасадом
put('ap_radar1_01', 45, -53.5, 0, what='радар', color=(180, 185, 190))
put('cxrf_watertwr', 34.5, -50, 0, what='водонапорная башня', color=(150, 120, 100))

# ---------------- северо-восток: два ангара (служебный и личный гараж), стоянка, заправка ----------------
put('mil_hangar1_', -46, -34, 0, what='служебный ангар', color=(140, 160, 130))
put('mil_hangar1_', -68, -34, 0, what='личный гараж (ангар)', color=(140, 160, 130))
P['OwnGarage'] = (-68, -14)                             # метка «на машине» перед воротами ангара
P['Fleet'] = (-36, -8)                                  # первая из 6 служебных машин, шаг 4 м на восток
P['Park'] = (-76, -6)                                   # свои машины со склада
put('cxrf_oiltank', -51, 54, 90, what='топливная цистерна', color=(160, 160, 165))
put('generator_big', -42, 55.5, 90, what='генератор', color=(120, 130, 110))

# ---------------- юго-запад: оружейная, тир, камера ----------------
put('cunts_ammun', 22, 30, 90, what='оружейная («Аммунация»)', color=(175, 140, 110))
P['Armory'] = (13, 30)
for i, (cu, cv) in enumerate(((11, 19), (11, 21.2), (13, 20), (11, 40), (13.2, 40.6))):
    put('kmilitary_crate', cu, cv, 15 * i, what='военный ящик', color=(90, 110, 80))
put('crates', 36, 22, 0, what='штабель ящиков', color=(120, 100, 70))
P['RangeStand'] = (44, 34.5); P['RangeTargets'] = (70, 34.5)
for i in range(4): put('target_frame', 70, 30 + i * 3.0, 90, what='мишень', color=(220, 220, 220))
put('ct_table', 45.5, 34.5, 90, what='стойка тира', color=(140, 110, 80))
for i in range(3): put('kmb_container_red', 75.5, 27.2 + i * 7.3, 0, what='пулеулавливатель (контейнеры)', color=(150, 70, 60))
cu, cv, hh = 30.0, 50.0, 6.7
P['Cell'] = (cu, cv); P['CellDrop'] = (cu - 9.0, cv)
for s in (-1, 1):
    put('tall_fence', cu - 3.35, cv + s * hh, 0, what='решётка камеры', color=(110, 110, 120)); put('tall_fence', cu + 3.35, cv + s * hh, 0, what='решётка камеры', color=(110, 110, 120))
    put('tall_fence', cu + s * hh, cv + 3.35, 90, what='решётка камеры', color=(110, 110, 120))
    if s > 0: put('tall_fence', cu + s * hh, cv - 3.35, 90, what='решётка камеры', color=(110, 110, 120))
x, y = W(cu + 4.0, cv + 4.0); put_origin('police_cell_bed', x, y, KZ + 0.05, 0, what='нары')
x, y = W(cu + 4.5, cv - 4.5); put_origin('police_cell_toilet', x, y, KZ + 0.05, 0, what='унитаз')

# ---------------- юго-восток: жилой городок (бытовки), зона отдыха ----------------
C = (200, 205, 195)
put('portakabin', -22, 24, 0, what='комната Артёма (бытовка)', color=(120, 150, 200)); P['Artyom'] = (-22, 20.2)
put('portakabin', -36, 24, 0, what='комната Маги (бытовка)', color=(200, 150, 120)); P['Maga'] = (-36, 20.2)
put('portakabin', -50, 24, 0, what='казарма 1', color=C)
put('portakabin', -36, 46, 0, what='казарма 2', color=C); put('portakabin', -50, 46, 0, what='казарма 3', color=C)
P['Rest'] = (-36, 35)
put('parkbench1', -38.5, 35, 0, what='лавка'); put('parkbench1', -33.5, 35, 0, what='лавка'); put('parktable1', -36, 35.2, 90, what='стол')
put('grill', -36, 38.5, 0, what='мангал')

# ---------------- вышки и прожекторы ----------------
T = (190, 80, 70)
for tu, tv in ((73, -54), (-73, -55.5), (73, 54), (-66, 55)): put('a51_spottower', tu, tv, 0, what='вышка', color=T)
P['Towers'] = [(73, -54), (-73, -55.5), (73, 54), (-66, 55)]
for lu, lv in ((21, 56.5), (-8.6, 56.5), (40, 17), (-42, 17), (34, -2), (-34, -2), (41, -9), (-58, -14), (-28, 35), (48, 44)):
    put('ws_floodlight', lu, lv, 0, what='прожектор', color=(235, 225, 150))

# ---------------- вид «Обычная» (0) и «Готэм» (1) ----------------
put('sfx_flag02', -9.5, 58.5, 0, dz=3.0, look=0, what='флажок у КПП'); put('sfx_flag02', 9.5, 58.5, 0, dz=3.0, look=0, what='флажок у КПП')
for du, dv in ((-9, 41), (18, 14), (-34, -8), (52, 8), (-58, 30), (60, 46)): put('cj_flame_drum', du, dv, 0, look=1, what='бочка с огнём', color=(230, 120, 40))
put('cj_skip_rubbish', 40, 56, 0, look=1, what='мусорный контейнер'); put('cj_dumpster', -62, 40, 90, look=1, what='мусорка')
put('cj_dumpster', 36, -4, 0, look=1, what='мусорка')

# ---------------- люди, маршрут, машины ----------------
STAFF = [('Охранник Бойко', 9, 57, 0), ('Охранник Мартинес', -9, 52, 180), ('Фрэнк', P['Frank'][0] + 2.0, P['Frank'][1] - 2.5, 180),
         ('Томми', -36, 40.2, 0), ('Сержант Ковальски', -46, -12, 90), ('Офицер Рейес', 44, -6, 0), ('Офицер Ли', 0, -3, 45),
         ('Офицер Карлос', 15, 26, 270), ('Офицер Ньюман', -30, 30, 135)]
ROUTE = [(-50, 16), (-10, 16), (-9, 40), (9, 40), (10, 16), (50, 16), (50, -5), (16, -5), (-16, -5), (-50, -5)]

# ====================================================================================================
def check():
    """Пересечения коробок построек (по осям, с учётом поворота на 90°) — грубая проверка плана."""
    boxes = []
    for p in pieces:
        if p['look'] == 0 or p['name'] in ('elecfence_bar', 'helipad', 'helipad01', 'satdishsml', 'vegasflag1', 'police_cell_bed', 'police_cell_toilet'): continue
        b = cols[p['name']]; r = math.radians(p['h'])
        cs = [(b[i], b[j]) for i in (0, 3) for j in (1, 4)]
        xs = [p['x'] + a * math.cos(r) - c * math.sin(r) for a, c in cs]; ys = [p['y'] + a * math.sin(r) + c * math.cos(r) for a, c in cs]
        boxes.append((p['what'], min(xs), min(ys), max(xs), max(ys)))
    bad = 0
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i], boxes[j]
            if a[0] == b[0] and 'решётка' in a[0]: continue
            ox = min(a[3], b[3]) - max(a[1], b[1]); oy = min(a[4], b[4]) - max(a[2], b[2])
            if ox > 0.3 and oy > 0.3: bad += 1; print('ПЕРЕСЕЧЕНИЕ: %s  и  %s  (%.1f x %.1f м)' % (a[0], b[0], ox, oy))
    for w, x0, y0, x1, y1 in boxes:
        if x0 < CX - 81.5 or x1 > CX + 81.5 or y0 < CY - 61.5 or y1 > CY + 61.5: bad += 1; print('ЗА ЗАБОРОМ:', w, round(CX - x1, 1), round(CX - x0, 1), round(CY - y1, 1), round(CY - y0, 1))
    print('построек: %d, проблем: %d' % (len(pieces), bad))

def header(path):
    o = ['// СГЕНЕРИРОВАНО work/base_plan.py — руками не править. План базы S.W.A.T. v2.', '#pragma once', 'namespace layout', '{']
    o.append('    constexpr float kRoofZ = kZ + %.2ff;                 // крыша штаба (башня здания полиции)' % ROOF)
    for k in ('Gate', 'Hq', 'Pad', 'Heli2', 'Frank', 'Lift', 'RoofLift', 'Med', 'OwnGarage', 'Fleet', 'Park', 'Armory', 'RangeStand', 'RangeTargets', 'Cell', 'CellDrop', 'Artyom', 'Maga', 'Rest'):
        o.append('    constexpr float k%sU = %.2ff, k%sV = %.2ff;' % (k, P[k][0], k, P[k][1]))
    o.append('    constexpr float kRangeV = %.2ff;' % P['RangeStand'][1])
    o.append('    struct Pt { float u, v; };')
    o.append('    constexpr Pt kTowers[] = { %s };' % ', '.join('{ %.1ff, %.1ff }' % t for t in P['Towers']))
    o.append('    constexpr Pt kRoute[] = { %s };' % ', '.join('{ %.1ff, %.1ff }' % t for t in ROUTE))
    o.append('    struct StaffSpot { const char* name; float u, v, planH; };')
    o.append('    constexpr StaffSpot kStaffSpots[] = { %s };' % ', '.join('{ "%s", %.2ff, %.2ff, %.1ff }' % s for s in STAFF))
    o.append('    // постройка: модель, мировые координаты начала модели, курс, вид (−1 всегда, 0 «Обычная», 1 «Готэм»), название')
    o.append('    struct Piece { int model; float x, y, z, h; int look; const char* what; };')
    o.append('    constexpr Piece kPieces[] = {')
    for p in pieces: o.append('        { %d, %.3ff, %.3ff, %.3ff, %.1ff, %d, "%s" },' % (p['model'], p['x'], p['y'], p['z'], p['h'], p['look'], p['what']))
    o.append('    };'); o.append('    constexpr int kPieceCount = (int)(sizeof(kPieces) / sizeof(kPieces[0]));'); o.append('}'); o.append('')
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(o))
    print('заголовок:', path, 'построек', len(pieces))

def render(folder, look=1):
    import render as R
    items = []
    g = np.array([[[CX - 84, CY - 64, KZ], [CX + 84, CY - 64, KZ], [CX + 84, CY + 64, KZ]], [[CX - 84, CY - 64, KZ], [CX + 84, CY + 64, KZ], [CX - 84, CY + 64, KZ]]], float)
    items.append((g, (120, 124, 128)))
    def strip(pts, w, col):                             # полоса старой дороги по узлам
        out = []
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            dx, dy = x1 - x0, y1 - y0; l = math.hypot(dx, dy) or 1; nx, ny = -dy / l * w, dx / l * w; z = KZ + 0.03
            a, b, c, d = (x0 + nx, y0 + ny, z), (x0 - nx, y0 - ny, z), (x1 - nx, y1 - ny, z), (x1 + nx, y1 + ny, z)
            out += [[a, b, c], [a, c, d]]
        items.append((np.array(out, float), col))
    strip([(2683.8, -2460), (2683.8, -2430.5), (2683.5, -2405.2)], 6, (88, 90, 94))
    strip([(2683.5, -2405.2), (2639.4, -2405.2), (2615.4, -2404.8), (2609, -2403), (2603.5, -2399.6), (2600, -2396.5)], 6, (88, 90, 94))
    strip([(2683.5, -2405.2), (2720, -2405), (2744, -2405.2), (2752, -2408.6), (2758, -2415.2), (2761.1, -2425.5), (2761, -2442.2), (2761, -2460)], 6, (88, 90, 94))
    labels = []; seen = set()
    for p in pieces:
        if p['look'] >= 0 and p['look'] != look: continue
        t = R.place(R.tris_of(p['name']), p['x'], p['y'], p['z'], p['h'])
        items.append((t, p['color']))
        key = p['what']
        big = ('штаб', 'медпункт (медцентр)', 'оружейная («Аммунация»)', 'служебный ангар', 'личный гараж (ангар)', 'диспетчерская КПП', 'домик охраны КПП',
               'комната Артёма (бытовка)', 'комната Маги (бытовка)', 'радар', 'топливная цистерна')
        if key in big and key not in seen and len(t):
            seen.add(key); c = t.reshape(-1, 3); labels.append((c[:, 0].mean(), c[:, 1].mean(), c[:, 2].max() + 1, key.split(' (')[0].upper()))
    for key in ('Pad', 'Heli2'):
        x, y = W(*P[key]); z = KZ + ROOF + 0.1
        items.append((np.array([[[x - 4, y - 3.6, z], [x + 4, y - 3.6, z], [x + 4, y + 3.6, z]], [[x - 4, y - 3.6, z], [x + 4, y + 3.6, z], [x - 4, y + 3.6, z]]], float), (235, 225, 90)))
        labels.append((x, y, z + 1, 'ПЛОЩАДКА'))
    for nm, (u, v) in (('ТИР', (57, 34.5)), ('КАМЕРА', P['Cell']), ('ВОРОТА', (0, 62)), ('ЗОНА ОТДЫХА', P['Rest']), ('СТОЯНКА', (-46, -8))):
        x, y = W(u, v); labels.append((x, y, KZ + 4, nm))
    os.makedirs(folder, exist_ok=True)
    c = (CX, CY, KZ)
    R.draw(items, size=(1800, 1400), az=0, top=True, labels=labels, center=c, base=4).save(folder + '/plan_top.png')
    R.draw(items, size=(1800, 1150), az=-28, el=33, labels=labels, center=c, base=4).save(folder + '/plan_3d_south.png')     # вид с юга (от ворот)
    R.draw(items, size=(1800, 1150), az=150, el=30, labels=labels, center=c, base=4).save(folder + '/plan_3d_north.png')
    print('рендер:', folder)

if __name__ == '__main__':
    check()
    if '--header' in sys.argv: header(sys.argv[sys.argv.index('--header') + 1])
    if '--render' in sys.argv: render(sys.argv[sys.argv.index('--render') + 1])
