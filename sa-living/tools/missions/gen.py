import pickle, sys, struct
exec(open('ana.py').read().split('JUMPS =')[0])
table = pickle.load(open('skip_table.pkl','rb'))
def fnv(b):
    h = 2166136261
    for x in b: h ^= x; h = (h * 16777619) & 0xffffffff
    return h
lines = []
for (mi, name, S, retOff, chk, sig) in table:
    base = offs[mi]; end = offs[mi+1] if mi+1 < len(offs) else len(data)
    ins, _ = dis(base, end)
    gxt = next((a[0][1] for p, oid, a in ins if oid == 0x0318 and a and a[0][0]=='s'), '')
    sigh = fnv(data[base:base+32])
    chk8 = ', '.join('0x%02X' % b for b in data[base+S:base+S+8])
    lines.append('    { %3d, 0x%08X, 0x%05X, 0x%02X, { %s }, "%s", "%s" },' % (mi, sigh, S, retOff, chk8, name or '', gxt))
hdr = '''#pragma once
// Пропуск миссий (Джарвис): где у каждой миссии блок «миссия пройдена». Получено разбором mainV1.scm GTA SA Android
// 2.10 (tools/scm, sa-living/tools/missions/gen.py): у этих миссий корень «GOSUB основная часть → проверка смерти/ареста →
// GOSUB уборка», а «пройдена» — отдельный блок (вход только прыжком, без RETURN/WAIT до REGISTER_MISSION_PASSED).
// Пропуск = перевести скрипт миссии на этот блок со стеком [возврат после первого GOSUB]: игра сама засчитает миссию
// (прогресс, награда, флаги сюжета), потом выполнит свою уборку — как при настоящем прохождении.
// Сверка в игре: подпись первых 32 байт миссии (FNV-1a) и 8 байт в начале блока — не совпало (другая версия
// или мод сценария) — пропуск запрещён.
#include <stdint.h>
struct MissionSkip { int index; uint32_t sig; uint32_t passOff; uint8_t retOff; uint8_t check[8]; const char* name; const char* gxt; };
static const MissionSkip kMissionSkip[] = {
''' + '\n'.join(lines) + '\n};\n'
open('/home/user/linux/sa-living/src/jarvis/mission_skip.h','w').write(hdr)
print(len(lines))
