"""Locate the PPC register save/restore helpers in a dumped XEX image. Usage: find_helpers.py <image.bin> <base>"""
import struct, sys
data = open(sys.argv[1], 'rb').read(); base = int(sys.argv[2], 16)
words = struct.unpack(f'>{len(data)//4}I', data[:len(data)//4*4])

def find(seq):
    return [base + i*4 for i in range(len(words)-len(seq)) if words[i:i+len(seq)] == tuple(seq)]

pats = {
    'savegprlr_14': [0xF9C1FF68], 'restgprlr_14': [0xE9C1FF68],
    'savefpr_14':   [0xD9C1FF70], 'restfpr_14':   [0xC9C1FF70],
    'savevmx_14':   [0x3960FEE0, 0x7DCB61CE], 'restvmx_14': [0x3960FEE0, 0x7DCB60CE],
}
for k, p in pats.items():
    print(k, [hex(a) for a in find(p)])
for a in find([0x3960FC00]):
    nxt = words[(a - base)//4 + 1]
    print('li r11,-0x400 at', hex(a), 'next', hex(nxt))
