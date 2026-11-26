import sys, struct
exec(open('ana.py').read().split('JUMPS =')[0])
JUMPS = {0x0002, 0x004C, 0x004D, 0x0050}
UNCOND = {0x0051, 0x0002, 0x004E}
table = []; bad_list = []
for mi, base in enumerate(offs):
    end = offs[mi+1] if mi + 1 < len(offs) else len(data)
    ins, _ = dis(base, end)
    idx = {p:i for i,(p,_,_) in enumerate(ins)}
    rel = lambda v: base + (-v) if v < 0 else v
    targets = {}
    for p, oid, a in ins:
        if oid in JUMPS and a and a[0][0] == 'i': targets.setdefault(rel(a[0][1]), []).append((p, oid))
    name = next((a[0][1] for p, oid, a in ins[:3] if oid == 0x03A4 and a and a[0][0]=='s'), None)
    # root: first gosub, then IF HAS_DEATHARREST..., GOTO_IF_FALSE, GOSUB fail, GOSUB cleanup, TERMINATE
    fg = next((i for i,(p,oid,a) in enumerate(ins[:4]) if oid == 0x0050), None)
    if fg is None: bad_list.append((mi, name, 'нет первого GOSUB')); continue
    seq = [ins[fg+k][1] for k in range(1, 7)]
    if seq != [0x00D6, 0x0112, 0x004D, 0x0050, 0x0050, 0x004E]: bad_list.append((mi, name, 'корень не типовой', [hex(x) for x in seq])); continue
    retOff = ins[fg+1][0] - base
    marks = [p for p, oid, a in ins if oid == 0x0318]
    if not marks: bad_list.append((mi, name, 'нет REGISTER_MISSION_PASSED')); continue
    found = None
    for pm in marks:
        cands = sorted(t for t in targets if t <= pm and pm - t < 0x300 and t in idx)   # самый ранний подходящий — начало блока
        for S in cands:
            i0 = idx[S]
            if i0 == 0 or ins[i0-1][1] not in UNCOND: continue          # вход только прыжком
            seg = ins[i0:idx[pm]]
            if any(o in (0x0051, 0x004E, 0x0001) for _, o, _ in seg): continue   # RETURN/TERMINATE/WAIT внутри — не блок
            jl = [rel(a[0][1]) for _, o, a in seg if o in JUMPS and a and a[0][0]=='i']
            if any(not (S <= j <= pm + 0x300) for j in jl): continue      # прыжки только внутри блока
            r = next((p for p, o, a in ins[idx[pm]:idx[pm]+60] if o == 0x0051), None)
            if r is None: continue
            found = (S, pm, r); break
        if found: break
    if not found: bad_list.append((mi, name, 'блок «пройдена» не отдельный')); continue
    S, pm, r = found
    sig = data[base:base+32]
    table.append((mi, name, S-base, retOff, data[S:S+8].hex(), sig.hex()))
for t in table: print('OK', t[0], t[1], hex(t[2]), hex(t[3]))
print('---- нельзя:')
for b in bad_list: print('NO', b)
print(len(table), 'пропускаемых')
import pickle; pickle.dump(table, open('skip_table.pkl','wb'))
