import json, struct, sys
lib = json.load(open(sys.argv[1]))
ops = {}
for ext in lib['extensions']:
    for c in ext['commands']:
        try: oid = int(c['id'], 16)
        except: continue
        params = (c.get('input') or []) + (c.get('output') or [])
        var = any(p.get('type') == 'arguments' for p in params)
        ops[oid] = (c['name'], c.get('num_params', len(params)), var)
data = open(sys.argv[2], 'rb').read()

def param(p):
    t = data[p]; p += 1
    if t == 0: return ('end', None), p
    if t == 1: return ('i', struct.unpack_from('<i', data, p)[0]), p + 4
    if t in (2, 3, 0xA, 0xB, 0x10, 0x11): return ('gv' if t in (2,0xA,0x10) else 'lv', struct.unpack_from('<H', data, p)[0]), p + 2
    if t == 4: return ('i', struct.unpack_from('<b', data, p)[0]), p + 1
    if t == 5: return ('i', struct.unpack_from('<h', data, p)[0]), p + 2
    if t == 6: return ('f', round(struct.unpack_from('<f', data, p)[0], 3)), p + 4
    if t in (7, 8, 0xC, 0xD, 0x12, 0x13): return ('arr', struct.unpack_from('<HH', data, p)), p + 6
    if t == 9: return ('s', data[p:p+8].split(b'\0')[0].decode('latin1')), p + 8
    if t == 0xE: n = data[p]; return ('s', data[p+1:p+1+n].decode('latin1')), p + 1 + n
    if t == 0xF: return ('s', data[p:p+16].split(b'\0')[0].decode('latin1')), p + 16
    raise ValueError(t)

def dis(start, end):
    p = start; out = []
    while p < end - 2:
        op = struct.unpack_from('<H', data, p)[0]
        neg = op & 0x8000; oid = op & 0x7FFF
        if oid not in ops: p += 1; continue
        name, n, var = ops[oid]
        q = p + 2; args = []
        try:
            if var:
                for i in range(n - 1 if n else 0):
                    a, q = param(q); args.append(a)
                while True:
                    a, q = param(q)
                    if a[0] == 'end': break
                    args.append(a)
            else:
                for i in range(n):
                    a, q = param(q); args.append(a)
        except Exception:
            p += 1; continue
        out.append((p, ('NOT ' if neg else '') + name, oid, args))
        p = q
    return out

if __name__ == '__main__':
    import pickle
    # header: segment jumps 0002 01 <int32>
    segs = []; p = 0
    while len(segs) < 8 and p < len(data):
        if data[p:p+3] != b'\x02\x00\x01': break
        tgt = struct.unpack_from('<i', data, p+3)[0]
        segs.append((p, tgt, data[p+7])); p = tgt
    print('segments', [(hex(a), hex(b), c) for a,b,c in segs])
    code = p
    print('code starts', hex(code))
    ins = dis(code, len(data))
    print('instructions', len(ins))
    pickle.dump((ins, [hex(s[1]) for s in segs]), open(sys.argv[3], 'wb'))
