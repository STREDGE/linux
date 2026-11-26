import struct, sys
data = open(sys.argv[1],'rb').read()
p = 0; segs=[]
while len(segs) < 8 and data[p:p+3] == b'\x02\x00\x01':
    tgt = struct.unpack_from('<i', data, p+3)[0]; segs.append((p, tgt)); p = tgt
print('segs', [(hex(a),hex(b)) for a,b in segs])
# mission seg = segs[2]: header at segs[2][0]+8
m = segs[2][0] + 8
mainSize, largest = struct.unpack_from('<II', data, m)
nmis, nexcl = struct.unpack_from('<HH', data, m+8)
largestLocals = struct.unpack_from('<I', data, m+12)[0]
offs = struct.unpack_from('<%dI' % nmis, data, m+16)
print('mainSize', hex(mainSize), 'largest', hex(largest), 'missions', nmis, 'excl', nexcl, 'locals', largestLocals)
print('first offs', [hex(o) for o in offs[:5]], 'last', hex(offs[-1]))
import pickle; pickle.dump((mainSize, list(offs)), open('missions.pkl','wb'))
