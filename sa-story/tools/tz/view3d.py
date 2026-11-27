import bpy, math, sys
from mathutils import Vector
FONT = "/home/user/linux/third_party/fonts/Rubik-Medium.ttf"
bpy.ops.wm.read_factory_settings(use_empty=True)
font = bpy.data.fonts.load(FONT)
mats = {}
def M(c, rough=0.8, emit=0.0):
    key = (c, rough, emit)
    if key in mats: return mats[key]
    m = bpy.data.materials.new("m%d" % len(mats)); m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*c, 1); b.inputs["Roughness"].default_value = rough
    if emit:
        b.inputs["Emission Color"].default_value = (*c, 1); b.inputs["Emission Strength"].default_value = emit
    mats[key] = m; return m
def hexc(h): h = h.lstrip("#"); return tuple((int(h[i:i+2], 16) / 255.0) ** 2.2 for i in (0, 2, 4))
def box(x, y, z, w, d, h, col, rough=0.8, emit=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=(x + w / 2, y + d / 2, z + h / 2))
    o = bpy.context.object; o.scale = (w, d, h); o.data.materials.append(M(hexc(col), rough, emit)); return o
def cyl(x, y, z, r, h, col, verts=24):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=h, vertices=verts, location=(x, y, z + h / 2))
    o = bpy.context.object; o.data.materials.append(M(hexc(col))); return o
def label(x, y, z, text, size=3.2, col="#ffffff"):
    bpy.ops.object.text_add(location=(x, y, z))
    t = bpy.context.object; t.data.body = text; t.data.font = font; t.data.size = size
    t.data.align_x = 'CENTER'; t.data.extrude = 0.05
    t.data.materials.append(M(hexc(col), 0.5, 2.0))
    c = t.constraints.new('TRACK_TO'); c.target = CAM; c.track_axis = 'TRACK_Z'; c.up_axis = 'UP_Y'
    return t

# камера заранее (для подписей)
bpy.ops.object.camera_add(location=(215, -95, 120))
CAM = bpy.context.object
d = Vector((78, 58, 0)) - CAM.location
CAM.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
CAM.data.lens = 30

# земля вокруг, дорога, бетон базы
box(-80, -80, -0.3, 330, 300, 0.3, "#3d4a3a")
box(-80, 122, -0.2, 330, 12, 0.25, "#2e3034")
for x in range(-70, 240, 8): box(x, 127.7, 0.06, 4, 0.6, 0.02, "#dddddd")
box(0, 0, -0.2, 160, 120, 0.22, "#6a6f76")
box(42, 46, 0.03, 68, 40, 0.02, "#7c8189")                     # плац
box(73, 96, 0.03, 14, 24, 0.02, "#55595f")                     # въезд
# забор
for (x0, y0, x1, y1) in [(0, 0, 160, 0), (0, 0, 0, 120), (160, 0, 160, 120), (0, 120, 73, 120), (87, 120, 160, 120)]:
    L = max(abs(x1 - x0), abs(y1 - y0))
    if x0 == x1: box(x0 - 0.2, y0, 0, 0.4, L, 3.2, "#9aa0a8", 0.4)
    else: box(x0, y0 - 0.2, 0, L, 0.4, 3.2, "#9aa0a8", 0.4)
# КПП
box(73, 119.6, 1.0, 14, 0.35, 0.35, "#e94d4d", 0.5)
box(72, 119.2, 0, 1.0, 1.0, 1.6, "#dddddd")
box(88, 109, 0, 6, 7, 3.2, "#2f6fb3"); box(87.5, 108.5, 3.2, 7, 8, 0.4, "#ffffff")
# штаб
box(55, 6, 0, 50, 26, 12, "#2b3550")
for fl in range(3):
    box(55.5, 31.9, 1.4 + fl * 4, 49, 0.3, 1.6, "#8fc3ff", 0.2, 0.6)
box(54.5, 5.5, 12, 51, 27, 0.6, "#3a4360")
cyl(80, 17, 12.6, 9, 0.1, "#3d4a6b", 48)
cyl(80, 17, 12.65, 9.0, 0.05, "#ffd34d", 48); cyl(80, 17, 12.66, 8.3, 0.06, "#3d4a6b", 48)
box(78, 13.5, 12.7, 1.2, 7, 0.05, "#ffd34d"); box(81, 13.5, 12.7, 1.2, 7, 0.05, "#ffd34d"); box(79, 16.4, 12.7, 2.2, 1.2, 0.05, "#ffd34d")
# вертолёт на крыше (условно)
heli = box(87, 20, 13.2, 7, 2.4, 2.4, "#1a1a1a", 0.3)
box(83, 20.8, 14, 4.5, 0.6, 0.6, "#1a1a1a"); box(86.5, 15, 15.9, 1.0, 12, 0.12, "#555555"); box(84.6, 20.6, 15.9, 5, 1.0, 0.12, "#555555")
# кабинет Фрэнка и вход на крышу
box(60, 32, 0, 9, 3, 3.5, "#7a5a2e"); box(101, 32, 0, 4, 3, 12, "#8a8f98")
# служебный гараж
box(4, 40, 0, 18, 54, 5.5, "#3f4752")
car_cols = ["#202020", "#1c2a44", "#e8e8e8", "#111111", "#e8e8e8", "#202020"]
for i in range(6):
    box(21.6, 40.6 + i * 9, 0.4, 0.6, 7.8, 4.4, "#16181b")           # проём
    box(23.5, 41.6 + i * 9, 0, 5.2, 2.3, 1.8 if i != 0 else 2.6, car_cols[i], 0.3)
box(4, 14, 0, 18, 20, 5, "#4b3f52")
# оружейная, тир
box(48, 100, 0, 16, 8, 3.2, "#4d5b2f", 0.5)
box(4, 106, 0, 40, 11, 0.05, "#55595f")
for k in range(4): box(6, 107.5 + k * 2.4, 0, 0.3, 1.2, 1.8, "#e94d4d")
box(38, 106, 0, 6, 11, 0.15, "#6b6f2e"); box(38, 106, 3.0, 6, 11, 0.25, "#6b6f2e")
for (x, y) in [(38, 106), (44, 106), (38, 117), (44, 117)]: box(x - 0.15, y - 0.15, 0, 0.3, 0.3, 3, "#888888")
# медпункт
box(100, 98, 0, 14, 11, 3.6, "#eeeeee"); box(106, 97.9, 2.0, 2, 0.1, 0.6, "#e94d4d"); box(106.7, 97.9, 1.3, 0.6, 0.1, 2.0, "#e94d4d")
box(116, 99, 0, 6, 9, 2.8, "#f4f4f4")
# жилые модули, отдых
box(134, 74, 0, 22, 11, 3.4, "#355e4a"); box(134, 50, 0, 22, 11, 3.4, "#5e3550")
box(140, 64, 0, 6, 2, 0.8, "#6b5b3e"); box(141, 67, 0, 4, 0.6, 0.5, "#6b5b3e")
# камера: решётка
box(130, 6, 0, 26, 18, 0.1, "#2a2a2a")
for i in range(27): box(130 + i, 6, 0, 0.12, 0.12, 3.6, "#c9c9c9", 0.3)
for i in range(27): box(130 + i, 23.9, 0, 0.12, 0.12, 3.6, "#c9c9c9", 0.3)
for j in range(19): box(130, 6 + j, 0, 0.12, 0.12, 3.6, "#c9c9c9", 0.3); box(156, 6 + j, 0, 0.12, 0.12, 3.6, "#c9c9c9", 0.3)
box(130, 6, 3.6, 26, 18, 0.15, "#c9c9c9", 0.3)
# флаг, вышки, фонари
cyl(76, 66, 0, 0.15, 10, "#dddddd"); box(76.1, 66, 8.2, 3.2, 0.05, 1.8, "#1c3a8a")
for (x, y) in [(2, 2), (158, 118), (158, 2), (2, 118)]:
    for dx in (-1.5, 1.5):
        for dy in (-1.5, 1.5): box(x + dx - 0.15, y + dy - 0.15, 0, 0.3, 0.3, 8, "#8a6d3b")
    box(x - 2.2, y - 2.2, 8, 4.4, 4.4, 2.4, "#8a6d3b")
    box(x - 0.4, y - 0.4, 10.4, 0.8, 0.8, 0.6, "#fff2a8", 0.3, 4.0)
# люди (условно)
def man(x, y, col):
    cyl(x, y, 0, 0.35, 1.5, col, 12); bpy.ops.mesh.primitive_uv_sphere_add(radius=0.3, location=(x, y, 1.75))
    bpy.context.object.data.materials.append(M(hexc(col)))
man(71, 116, "#4fd15a"); man(89, 106, "#4fd15a"); man(64.5, 37, "#ff9f43"); man(149, 66, "#5aaaff")
for (x, y) in [(50, 60), (95, 75), (30, 85), (120, 30), (40, 30)]: man(x, y, "#c8c8c8")

# подписи
L = [(80, 125, 5, "КПП"), (80, 19, 22, "ШТАБ + ВЕРТОЛЁТНАЯ ПЛОЩАДКА"), (13, 67, 9, "СЛУЖЕБНЫЙ ГАРАЖ"), (13, 24, 8, "ЛИЧНЫЙ ГАРАЖ"),
     (56, 104, 6, "ОРУЖЕЙНАЯ"), (22, 111, 4, "ТИР"), (110, 103, 6.5, "МЕДПУНКТ"), (145, 79, 6, "АРТЁМ"), (145, 55, 6, "МАГА"),
     (143, 15, 7, "КАМЕРА"), (64, 34, 6, "ФРЭНК"), (76, 70, 12, "ПЛАЦ")]
for (x, y, z, s) in L: label(x, y, z, s, 3.0)

# свет и небо
bpy.ops.object.light_add(type='SUN', rotation=(math.radians(48), math.radians(8), math.radians(-35)))
bpy.context.object.data.energy = 3.2
w = bpy.data.worlds.new("w"); bpy.context.scene.world = w; w.use_nodes = True
w.node_tree.nodes["Background"].inputs[0].default_value = (0.42, 0.55, 0.78, 1); w.node_tree.nodes["Background"].inputs[1].default_value = 0.9
sc = bpy.context.scene; sc.camera = CAM
sc.render.engine = 'CYCLES'; sc.cycles.device = 'CPU'; sc.cycles.samples = 32
sc.render.resolution_x = 1600; sc.render.resolution_y = 900
sc.render.filepath = sys.argv[-1]
bpy.ops.render.render(write_still=True)
print("render ok")
