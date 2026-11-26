import json, struct, sys, pickle
lib = json.load(open(sys.argv[1])); data = open(sys.argv[2],'rb').read()
mainSize, offs = pickle.load(open('missions.pkl','rb'))
ops = {}
for ext in lib['extensions']:
    for c in ext['commands']:
        try: oid = int(c['id'], 16)
        except: continue
        params = (c.get('input') or []) + (c.get('output') or [])
        var = any(p.get('type') == 'arguments' for p in params)
        ops[oid] = (c['name'], c.get('num_params', len(params)), var)
def param(p):
    t = data[p]; p += 1
    if t == 0: return ('end', None), p
    if t == 1: return ('i', struct.unpack_from('<i', data, p)[0]), p + 4
    if t in (2, 3, 0xA, 0xB, 0x10, 0x11): return ('gv' if t in (2,0xA,0x10) else 'lv', struct.unpack_from('<H', data, p)[0]), p + 2
    if t == 4: return ('i', struct.unpack_from('<b', data, p)[0]), p + 1
    if t == 5: return ('i', struct.unpack_from('<h', data, p)[0]), p + 2
    if t == 6: return ('f', 0.0), p + 4
    if t in (7, 8, 0xC, 0xD, 0x12, 0x13): return ('arr', None), p + 6
    if t == 9: return ('s', data[p:p+8].split(b'\0')[0].decode('latin1')), p + 8
    if t == 0xE: n = data[p]; return ('s', data[p+1:p+1+n].decode('latin1')), p + 1 + n
    if t == 0xF: return ('s', data[p:p+16].split(b'\0')[0].decode('latin1')), p + 16
    raise ValueError(t)
bad=[0]
def dis(start, end):
    p = start; out = []
    while p < end - 1:
        op = struct.unpack_from('<H', data, p)[0]; oid = op & 0x7FFF
        if oid not in ops: p += 1; bad[0] += 1; continue
        name, n, var = ops[oid]; q = p + 2; args = []
        try:
            if var:
                for i in range(n - 1 if n else 0): a, q = param(q); args.append(a)
                while True:
                    a, q = param(q)
                    if a[0] == 'end': break
                    args.append(a)
            else:
                for i in range(n): a, q = param(q); args.append(a)
        except Exception:
            p += 1; bad[0] += 1; continue
        out.append((p, oid, args)); p = q
    return out, p
JUMPS = {0x0002, 0x004C, 0x004D, 0x0050}
res = []
for mi, base in enumerate(offs):
    end = offs[mi+1] if mi + 1 < len(offs) else len(data)
    ins, stop = dis(base, end)
    rel = lambda v: base + (-v) if v < 0 else v   # mission jumps negative = from base
    targets = set()
    for p, oid, a in ins:
        if oid in JUMPS and a and a[0][0] == 'i': targets.add(rel(a[0][1]))
    name = next((a[0][1] for p, oid, a in ins if oid == 0x03A4 and a and a[0][0]=='s'), '?')   # SCRIPT_NAME
    firstGosub = next(((p, rel(a[0][1])) for p, oid, a in ins if oid == 0x0050), None)
    passMarks = [p for p, oid, a in ins if oid == 0x0318 or (oid == 0x01E3 and a and a[0][0]=='s' and a[0][1].startswith('M_PASS'))]
    if not passMarks: res.append((mi, name, 'NOPASS', None)); continue
    pm = min(passMarks)
    idx = {p:i for i,(p,_,_) in enumerate(ins)}
    # passed block start: nearest jump target <= pm with no RETURN/TERMINATE between
    cands = sorted(t for t in targets if t <= pm)
    start = None
    for t in reversed(cands):
        if t not in idx: continue
        ok = all(ins[k][1] not in (0x0051, 0x004E) for k in range(idx[t], idx[pm]))
        if ok: start = t
        else: break
    # block end: first RETURN after pm
    ret = next((p for p, oid, a in ins if p > pm and oid == 0x0051), None)
    # what jumps to start
    via = [ (hex(p), oid) for p, oid, a in ins if oid in JUMPS and a and a[0][0]=='i' and rel(a[0][1]) == start]
    res.append((mi, name, hex(start - base) if start else None, firstGosub and hex(firstGosub[0]-base), len(via), stop >= end - 2))
for r in res: print(r)
