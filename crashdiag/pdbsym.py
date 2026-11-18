"""Map RVAs to public symbols: pdbsym.py <pdb> <dll on disk> <hex rva>..."""
import struct, sys, bisect

def msf_streams(d):
    bs, = struct.unpack_from('<I', d, 32)
    _, nblocks, dirbytes, _, mapaddr = struct.unpack_from('<IIIII', d, 36)
    ndir = (dirbytes + bs - 1) // bs
    dirblocks = struct.unpack_from('<%dI' % ndir, d, mapaddr * bs)
    dirdata = b''.join(d[b * bs:(b + 1) * bs] for b in dirblocks)[:dirbytes]
    n, = struct.unpack_from('<I', dirdata, 0)
    sizes = struct.unpack_from('<%dI' % n, dirdata, 4)
    off = 4 + 4 * n
    streams = []
    for s in sizes:
        if s == 0xFFFFFFFF: s = 0
        nb = (s + bs - 1) // bs
        blocks = struct.unpack_from('<%dI' % nb, dirdata, off); off += 4 * nb
        streams.append(b''.join(d[b * bs:(b + 1) * bs] for b in blocks)[:s])
    return streams

def sections(dll):
    d = open(dll, 'rb').read()
    pe, = struct.unpack_from('<I', d, 0x3c)
    n, = struct.unpack_from('<H', d, pe + 6); optsz, = struct.unpack_from('<H', d, pe + 20)
    return [struct.unpack_from('<I', d, pe + 24 + optsz + 40 * i + 12)[0] for i in range(n)]

def main():
    pdb, dll = sys.argv[1], sys.argv[2]
    st = msf_streams(open(pdb, 'rb').read())
    dbi = st[3]
    symrec, = struct.unpack_from('<H', dbi, 20)
    secva = sections(dll)
    rec = st[symrec]; o = 0; syms = []
    while o + 4 <= len(rec):
        ln, kind = struct.unpack_from('<HH', rec, o)
        if kind == 0x110E:
            _, off, seg = struct.unpack_from('<IIH', rec, o + 4)
            name = rec[o + 14:rec.index(b'\0', o + 14)].decode('utf-8', 'replace')
            if 1 <= seg <= len(secva): syms.append((secva[seg - 1] + off, name))
        o += ln + 2
    syms.sort(); keys = [s[0] for s in syms]
    for a in sys.argv[3:]:
        r = int(a, 16); i = bisect.bisect_right(keys, r) - 1
        print(hex(r), syms[i][1] + '+0x%x' % (r - syms[i][0]) if i >= 0 else '?')

main()
