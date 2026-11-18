import struct, sys
d = open(sys.argv[1], 'rb').read()
pe, = struct.unpack_from('<I', d, 0x3c)
nsec, = struct.unpack_from('<H', d, pe + 6); optsz, = struct.unpack_from('<H', d, pe + 20)
opt = pe + 24; magic, = struct.unpack_from('<H', d, opt)
dd = opt + (112 if magic == 0x20b else 96)
erva, esz = struct.unpack_from('<II', d, dd)
secs = []
for i in range(nsec):
    o = opt + optsz + 40 * i
    vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8); secs.append((va, max(vs, rs), ra))
def off(rva):
    for va, sz, ra in secs:
        if va <= rva < va + sz: return ra + rva - va
e = off(erva)
nf, nn, af, an, ao = struct.unpack_from('<IIIII', d, e + 20)
funcs = [struct.unpack_from('<I', d, off(af) + 4 * i)[0] for i in range(nf)]
syms = []
for i in range(nn):
    nr, = struct.unpack_from('<I', d, off(an) + 4 * i); o, = struct.unpack_from('<H', d, off(ao) + 2 * i)
    s = d[off(nr):d.index(b'\0', off(nr))].decode()
    syms.append((funcs[o], s))
syms.sort()
for t in sys.argv[2:]:
    t = int(t, 16); best = max((x for x in syms if x[0] <= t), default=None)
    print(hex(t), '->', best[1] if best else None, '+0x%x' % (t - best[0]) if best else '')
