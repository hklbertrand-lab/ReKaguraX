"""Résume les écritures [watch] du journal : une ligne par pile distincte (hors allocations). Usage: watch_report.py run.log syms.txt [filtre]"""
import bisect, re, sys
syms = []
for l in open(sys.argv[2]):
    a, _, n = l.split(None, 2); syms.append((int(a, 16) - 0x140000000, n.strip()))
addrs = [a for a, _ in syms]
def sym(o):
    i = bisect.bisect_right(addrs, o) - 1
    return syms[i][1] if 0 <= i and o < 0x2000000 else '?'
blocks, cur = [], None
for l in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if '[watch] écriture' in l: cur = [l.split()[2], []]; blocks.append(cur)
    elif '[watch]   pile' in l and cur: cur[1].append(sym(int(l.split('exe+')[1], 16)))
seen = {}
for addr, stack in blocks:
    if sys.argv[3:] and not re.match(sys.argv[3], addr): continue
    if any('Bump' in s for s in stack): continue
    key = ' <- '.join(s for s in stack[1:] if s != '?')
    seen.setdefault(key, []).append(addr)
for k, v in seen.items(): print(len(v), v[0], v[-1], ':', k)
