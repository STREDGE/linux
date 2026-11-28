# Высоты поверхностей модели над точкой (в осях модели): python -I hat.py имя x,y x,y ...
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, render as R
def heights(name, x, y):
    t = R.tris_of(name); out = []
    for a, b, c in t:
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-9: continue
        l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d
        l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d
        l3 = 1 - l1 - l2
        if l1 >= -1e-6 and l2 >= -1e-6 and l3 >= -1e-6: out.append(round(float(l1 * a[2] + l2 * b[2] + l3 * c[2]), 2))
    return sorted(set(out))
if __name__ == '__main__':
    for p in sys.argv[2:]:
        x, y = map(float, p.split(',')); print(p, heights(sys.argv[1], x, y))
