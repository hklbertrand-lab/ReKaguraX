"""Minimal STFS (LIVE/PIRS/CON) extractor. Usage: stfs_extract.py <package> <outdir>"""
import os, struct, sys

def i24le(b): return b[0] | b[1] << 8 | b[2] << 16
def i24be(b): return b[0] << 16 | b[1] << 8 | b[2]

class Stfs:
    def __init__(s, path):
        s.f = open(path, 'rb')
        h = s.read(0, 0x400)
        s.magic = h[:4]
        hdr_size = struct.unpack('>I', h[0x340:0x344])[0]
        s.base = (hdr_size + 0xFFF) & 0xF000
        sep = h[0x37B]
        s.shift = 0 if sep & 1 else 1
        s.ft_count = struct.unpack('<H', h[0x37C:0x37E])[0]
        s.ft_block = i24le(h[0x37E:0x381])

    def read(s, off, n):
        s.f.seek(off); return s.f.read(n)

    def data_block(s, b):
        r = (((b + 0xAA) // 0xAA) << s.shift) + b
        if b < 0xAA: return r
        if b < 0x70E4: return r + (((b + 0x70E4) // 0x70E4) << s.shift)
        return (1 << s.shift) + r + (((b + 0x70E4) // 0x70E4) << s.shift)

    def block_off(s, b): return s.base + (s.data_block(b) << 12)

    def hash_off(s, b):
        n = (b // 0xAA) * 0xAB
        n += ((b // 0x70E4) + 1) << s.shift
        if b // 0x70E4: n += 1 << s.shift
        if b < 0xAA: n = 0
        return s.base + (n << 12) + (b % 0xAA) * 0x18

    def next_block(s, b):
        return i24be(s.read(s.hash_off(b) + 0x15, 3))

    def chain(s, start, count, consecutive):
        b = start
        for _ in range(count):
            yield b
            b = b + 1 if consecutive else s.next_block(b)

    def entries(s):
        raw = b''.join(s.read(s.block_off(b), 0x1000)
                       for b in s.chain(s.ft_block, s.ft_count, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            fl = e[0x28]
            if not fl & 0x3F: continue
            out.append(dict(name=e[:fl & 0x3F].decode('latin-1'), dir=bool(fl & 0x80),
                            cons=bool(fl & 0x40), blocks=i24le(e[0x29:0x2C]),
                            start=i24le(e[0x2F:0x32]), parent=struct.unpack('>h', e[0x32:0x34])[0],
                            size=struct.unpack('>I', e[0x34:0x38])[0], idx=i // 0x40))
        return out

    def extract(s, outdir):
        ents = s.entries(); byidx = {e['idx']: e for e in ents}
        def path(e):
            p = [e['name']]
            while e['parent'] != -1: e = byidx[e['parent']]; p.insert(0, e['name'])
            return os.path.join(outdir, *p)
        for e in ents:
            p = path(e)
            if e['dir']: os.makedirs(p, exist_ok=True); continue
            os.makedirs(os.path.dirname(p), exist_ok=True)
            left = e['size']
            with open(p, 'wb') as o:
                for b in s.chain(e['start'], e['blocks'], e['cons']):
                    o.write(s.read(s.block_off(b), min(0x1000, left))); left -= 0x1000
                    if left <= 0: break
            print(f"{e['size']:>10}  {os.path.relpath(p, outdir)}")

if __name__ == '__main__':
    st = Stfs(sys.argv[1]); print(st.magic, hex(st.base), 'shift', st.shift)
    st.extract(sys.argv[2])
