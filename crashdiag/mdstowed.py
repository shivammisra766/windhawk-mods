import struct, sys

def main(path):
    d = open(path, 'rb').read()
    sig, ver, nstreams, dirrva = struct.unpack_from('<4sIII', d, 0)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from('<III', d, dirrva + 12 * i)
        streams.setdefault(t, (size, rva))
    # modules
    mods = []
    n, = struct.unpack_from('<I', d, streams[4][1])
    off = streams[4][1] + 4
    for i in range(n):
        base, size, _, _, namerva = struct.unpack_from('<QIIII', d, off)
        ln, = struct.unpack_from('<I', d, namerva)
        name = d[namerva + 4:namerva + 4 + ln].decode('utf-16le')
        mods.append((base, size, name))
        off += 108
    def sym(a):
        for b, s, nm in mods:
            if b <= a < b + s:
                return '%s+0x%x' % (nm.split('\\')[-1], a - b)
        return None
    # memory
    ranges = []
    if 9 in streams:
        cnt, baserva = struct.unpack_from('<QQ', d, streams[9][1])
        o = streams[9][1] + 16
        r = baserva
        for i in range(cnt):
            st, sz = struct.unpack_from('<QQ', d, o + 16 * i)
            ranges.append((st, sz, r)); r += sz
    if 5 in streams:
        cnt, = struct.unpack_from('<I', d, streams[5][1])
        for i in range(cnt):
            st, sz, rva = struct.unpack_from('<QII', d, streams[5][1] + 4 + 16 * i)
            ranges.append((st, sz, rva))
    def rd(a, n):
        for st, sz, rva in ranges:
            if st <= a and a + n <= st + sz:
                return d[rva + a - st: rva + a - st + n]
        return None
    def q(a):
        b = rd(a, 8); return struct.unpack('<Q', b)[0] if b else None
    # exception
    eo = streams[6][1]
    tid, = struct.unpack_from('<I', d, eo)
    code, flags, rec, addr, npar = struct.unpack_from('<IIQQI', d, eo + 8)
    info = struct.unpack_from('<15Q', d, eo + 8 + 32)
    print('code 0x%x at %s, tid %d' % (code, sym(addr), tid))
    print('windhawk modules:', [m[2].split('\\')[-1] for m in mods if 'windhawk' in m[2].lower() or '_1.' in m[2] or 'local@' in m[2]])
    if code == 0xc000027b:
        arr, count = info[0], info[1]
        for i in range(count):
            p = q(arr + 8 * i)
            if p is None: print('unreadable ptr'); continue
            h = rd(p, 48)
            if not h: print('unreadable stowed'); continue
            size, sg, hr, form_tid = struct.unpack_from('<I4sII', h, 0)
            print('stowed %d: sig %s hr 0x%08x form %d' % (i, sg, hr, form_tid & 3))
            if form_tid & 3 == 1:
                eaddr, wsz, words, st = struct.unpack_from('<QIIQ', h, 16); print('  addr', sym(eaddr), 'frames', words)
                for k in range(min(words, 60)):
                    a = q(st + wsz * k)
                    if a: print('   ', sym(a) or hex(a))

for p in ([] if sys.argv[1]=="--scan" else sys.argv[1:]):
    print('=====', p.split('\\')[-1]); main(p)

def stackscan(path, limit=70):
    d = open(path, 'rb').read()
    _, _, ns, dr = struct.unpack_from('<4sIII', d, 0)
    st = {}
    for i in range(ns):
        t, size, rva = struct.unpack_from('<III', d, dr + 12 * i); st.setdefault(t, (size, rva))
    mods = []
    n, = struct.unpack_from('<I', d, st[4][1]); off = st[4][1] + 4
    for i in range(n):
        base, size, _, _, nr = struct.unpack_from('<QIIII', d, off)
        ln, = struct.unpack_from('<I', d, nr); mods.append((base, size, d[nr+4:nr+4+ln].decode('utf-16le').split(chr(92))[-1])); off += 108
    tid, = struct.unpack_from('<I', d, st[6][1])
    n, = struct.unpack_from('<I', d, st[3][1])
    for i in range(n):
        t, _, _, _, teb, sst, ssz, srva = struct.unpack_from('<IIIIQQII', d, st[3][1] + 4 + 48 * i)
        if t != tid: continue
        out = []
        for k in range(0, ssz - 7, 8):
            a, = struct.unpack_from('<Q', d, srva + k)
            for b, s, nm in mods:
                if b <= a < b + s and not nm.lower().endswith(('.exe',)) :
                    out.append('%s+0x%x' % (nm, a - b)); break
        print('\n'.join(out[:limit]))
        print('mod hits:', sorted(set(x.split('+')[0] for x in out if '_1.' in x or 'local@' in x or '_3.' in x or '_0.' in x)))

if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == '--scan':
    for p in sys.argv[2:]:
        print('=====', p); stackscan(p)
