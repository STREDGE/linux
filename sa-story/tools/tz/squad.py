import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import sys
fig = plt.figure(figsize=(15, 10), dpi=110); fig.patch.set_facecolor("#14181f")
ax = fig.add_axes([0, 0, 1, 1]); ax.set_xlim(0, 150); ax.set_ylim(0, 100); ax.axis("off")
def B(x, y, w, h, fc, title, body, tc="white"):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.6,rounding_size=2", fc=fc, ec="#e8e8e8", lw=1.6))
    ax.text(x + w / 2, y + h - 3.2, title, ha="center", va="center", fontsize=13, fontweight="bold", color=tc)
    ax.text(x + 2, y + h - 7.5, body, ha="left", va="top", fontsize=10, color="#f0f0f0", linespacing=1.5)
def A(x0, y0, x1, y1, text="", c="#ffd34d", rad=0.0, tx=None, ty=None):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=22, color=c, lw=2.2, connectionstyle="arc3,rad=%s" % rad))
    if text: ax.text(tx if tx is not None else (x0 + x1) / 2, ty if ty is not None else (y0 + y1) / 2, text, ha="center", va="center", fontsize=9.5, color=c,
                     bbox=dict(boxstyle="round,pad=0.25", fc="#14181f", ec="none"))
ax.text(75, 96, "ОТРЯД С ИИ — как устроен (черновик ТЗ)", ha="center", fontsize=17, fontweight="bold", color="white")

B(3, 62, 30, 26, "#2f6fb3", "ТЫ", "голос / текст:\n«отряд, за мной»\n«Томми, за руль»\n«догоните его»\nкнопки отряда\nна экране")
B(48, 58, 50, 30, "#5e3550", "ГОЛОВА — Haiku (SA Living)", "ОДИН запрос на весь отряд:\n• понимает приказ и обстановку\n• решает за всех троих, кто что делает\n• у каждого свой характер и голос\n• думает при событии, а не каждый кадр\n• помнит, что вы делали (сохраняется)")
B(113, 62, 34, 26, "#355e4a", "РЕПЛИКИ", "голоса SA Living\nпо очереди,\nне хором\n\nсубтитры внизу")
B(48, 8, 50, 36, "#3f4752", "РУКИ — сама игра (каждый кадр)", "родной водитель, пилот и стрелок GTA:\n• ходить, бежать, за тобой, ждать, охранять\n• машина: за руль, везти, погоня, таран,\n   стрельба из окна\n• вертолёт: взлёт, лететь, кружить, сесть\n• лодка; бой: цели, прикрыть, отступить\n• задержать, вызвать подмогу, дать звёзды")
B(3, 8, 34, 40, "#4d5b2f", "SA STORY (режим S.W.A.T.)", "сообщает SA Living:\n• режим включён → отряд есть\n• где база, медпункт,\n   оружейная, камера\n• рация: подмога\n• розыск NPC: звёзды\n\nбез режима — отряда нет")
B(113, 8, 34, 40, "#6b3a2e", "ПРАВИЛА", "• без читов: машины и оружие\n   только как у игрока\n• погиб → через время\n   на базе в медпункте\n• сохранение/загрузка —\n   отряд остаётся\n• нет интернета — просто\n   идут за тобой и дерутся")

A(33, 75, 48, 75, "приказ")
A(98, 75, 113, 75, "ответ")
A(70, 58, 70, 44, "приказы\nкаждому", tx=62)
A(80, 44, 80, 58, "события: приехали,\nобстрел, цель ушла,\nранен, погиб", c="#5aaaff", tx=92)
A(37, 30, 48, 30, "место, рация,\nзвёзды", c="#4fd15a", tx=42.5, ty=37)
plt.savefig(sys.argv[1], facecolor=fig.get_facecolor()); print("ok")
