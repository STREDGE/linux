#!/usr/bin/env python3
"""Разбор подробного лога SA Living и SA Coop (trace_*.txt) — формат строк одинаковый.

  trace_tool.py ФАЙЛ summary                 — сводка: сессия, события по видам, вылеты, метки, FPS
  trace_tool.py ФАЙЛ marks                   — все отметки игрока
  trace_tool.py ФАЙЛ around ВРЕМЯ [до] [после] — всё за [до] с до и [после] с после времени (ЧЧ:ММ:СС), по умолчанию 60 и 10
  trace_tool.py ФАЙЛ mark N [до] [после]     — то же вокруг N-й отметки (с 1)
  trace_tool.py ФАЙЛ entity P#1234|V#567     — вся история одного педа или машины
  trace_tool.py ФАЙЛ tags ТЕГ[,ТЕГ…]          — только строки этих тегов (например РОЗЫСК,АРЕСТ,ЗАДАЧА)
  trace_tool.py ФАЙЛ map ВРЕМЯ out.png [радиус] — карта: игрок, педы, машины в ближайшем к ВРЕМЕНИ снимке (SA Living)
  trace_tool.py merge ХОСТ ДРУГ [сдвиг_с] [ТЕГ,…] — SA Coop: логи двух телефонов вместе по времени (Х/Д в начале строки);
                                                сдвиг_с — насколько часы друга впереди хоста (по умолчанию 0)
"""
import re
import sys
from collections import Counter

# с 1.4.8 / 0.5.7 — общий файл SA_LOG.txt: строка начинается с метки мода «К »/«И »/«Т » (кооп, SA Living, Tailscale)
LINE = re.compile(r'^(?:[КИТ] )?(\d\d:\d\d:\d\d\.\d\d\d|XX:XX:XX\.XXX) \[([^\]]+)\] (.*)$')
POS = re.compile(r'\((-?\d+\.?\d*),(-?\d+\.?\d*),(-?\d+\.?\d*)\)')


def secs(ts):
    if ts.startswith('XX'):
        return None
    h, m, s = ts.split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


def load(path):
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            m = LINE.match(raw.rstrip('\n'))
            if m:
                out.append((m.group(1), secs(m.group(1)), m.group(2), m.group(3)))
            elif out:  # продолжение многострочного текста
                ts, t, tag, txt = out[-1]
                out[-1] = (ts, t, tag, txt + '\n' + raw.rstrip('\n'))
    return out


def show(rows):
    for ts, _, tag, txt in rows:
        print(f'{ts} [{tag}] {txt}')


def window(rows, center, before, after):
    return [r for r in rows if r[1] is None or (center - before <= r[1] <= center + after)]


def merge(a, b, shift=0.0, tags=None):
    ra = [('Х',) + r for r in load(a)]
    rb = [('Д',) + r for r in load(b)]
    allr = []
    for who, ts, t, tag, txt in ra + rb:
        if tags and tag not in tags:
            continue
        key = (t - shift if who == 'Д' else t) if t is not None else 1e12
        allr.append((key, who, ts, tag, txt))
    allr.sort(key=lambda r: r[0])
    for _, who, ts, tag, txt in allr:
        print(f'{who} {ts} [{tag}] {txt}')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    if sys.argv[1] == 'merge':
        if len(sys.argv) < 4:
            print(__doc__)
            return
        shift = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
        tags = set(sys.argv[5].split(',')) if len(sys.argv) > 5 else None
        merge(sys.argv[2], sys.argv[3], shift, tags)
        return
    rows = load(sys.argv[1])
    cmd = sys.argv[2]
    a = sys.argv[3:]
    if cmd == 'summary':
        for r in rows:
            if r[2] == 'СЕССИЯ':
                print(r[3])
        c = Counter(r[2] for r in rows)
        print('\nстрок по тегам:')
        for k, v in c.most_common():
            print(f'  {k:14} {v}')
        print('\nвылеты:'); show([r for r in rows if r[2] == 'ВЫЛЕТ'])
        print('\nметки:'); show([r for r in rows if r[2] == 'ОТМЕТКА' and 'ОТМЕТКА ИГРОКА' in r[3]])
        print('\nподвисания:'); show([r for r in rows if r[2] == 'ПОДВИСАНИЕ'][:30])
        fps = [r for r in rows if r[2] == 'FPS']
        if fps:
            print('\nFPS (первые и последние):'); show(fps[:3] + fps[-3:])
    elif cmd == 'marks':
        show([r for r in rows if r[2] == 'ОТМЕТКА' and 'ОТМЕТКА ИГРОКА' in r[3]])
    elif cmd in ('around', 'mark'):
        if cmd == 'mark':
            marks = [r for r in rows if r[2] == 'ОТМЕТКА' and 'ОТМЕТКА ИГРОКА' in r[3]]
            center = marks[int(a[0]) - 1][1]
            a = a[1:]
        else:
            center = secs(a[0] + ('.000' if '.' not in a[0] else ''))
            a = a[1:]
        before = float(a[0]) if len(a) > 0 else 60
        after = float(a[1]) if len(a) > 1 else 10
        show(window(rows, center, before, after))
    elif cmd == 'entity':
        key = a[0]
        pat = re.compile(re.escape(key) + r'(?!\d)')
        show([r for r in rows if pat.search(r[3])])
    elif cmd == 'tags':
        tags = set(a[0].split(','))
        show([r for r in rows if r[2] in tags])
    elif cmd == 'map':
        from PIL import Image, ImageDraw
        center = secs(a[0] + ('.000' if '.' not in a[0] else ''))
        out = a[1]
        radius = float(a[2]) if len(a) > 2 else 80
        # последнее известное состояние каждого объекта к моменту center
        state = {}
        player = None
        for ts, t, tag, txt in rows:
            if t is None or t > center:
                continue
            if tag == 'ИГРОК':
                m = POS.search(txt)
                if m:
                    player = tuple(map(float, m.groups()))
            elif tag in ('ПЕД', 'ПЕД+', 'МАШ', 'МАШ+'):
                ref = txt.split()[0]
                m = POS.search(txt)
                if m:
                    state[ref] = (tuple(map(float, m.groups())), txt)
            elif tag in ('ПЕД-', 'МАШ-'):
                state.pop(txt.split()[0], None)
        if not player:
            print('нет позиции игрока до этого времени')
            return
        size = 900
        img = Image.new('RGB', (size, size), (24, 26, 32))
        d = ImageDraw.Draw(img)
        sc = size / (2 * radius)

        def xy(p):
            return ((p[0] - player[0]) * sc + size / 2, size / 2 - (p[1] - player[1]) * sc)
        for r in range(10, int(radius) + 1, 10):
            d.ellipse([size / 2 - r * sc, size / 2 - r * sc, size / 2 + r * sc, size / 2 + r * sc], outline=(45, 48, 56))
        for ref, (p, txt) in state.items():
            if abs(p[0] - player[0]) > radius or abs(p[1] - player[1]) > radius:
                continue
            x, y = xy(p)
            if ref.startswith('V#'):
                col = (90, 170, 255) if 'drv=' in txt else (70, 90, 120)
                d.rectangle([x - 6, y - 4, x + 6, y + 4], outline=col, width=2)
            else:
                col = (255, 200, 80) if '[наш#' in txt else (200, 200, 200)
                if ' t=6 ' in txt:
                    col = (80, 120, 255)  # коп
                if ' st=54' in txt or ' st=55' in txt:
                    col = (120, 40, 40)
                d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=col)
            d.text((x + 6, y - 6), ref, fill=(150, 150, 150))
        x, y = xy(player)
        d.ellipse([x - 7, y - 7, x + 7, y + 7], fill=(80, 220, 100))
        d.text((10, 10), f'{a[0]}  игрок ({player[0]:.0f},{player[1]:.0f},{player[2]:.0f})  радиус {radius:.0f} м, круги через 10 м', fill=(230, 230, 230))
        img.save(out)
        print('сохранено', out)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
