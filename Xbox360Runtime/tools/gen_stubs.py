"""Génère imports_stub.cpp : chaque import noyau non défini dans kernel.cpp journalise et renvoie 0."""
import re, sys
shared, kernel, out = sys.argv[1:4]
names = re.findall(r'PPC_EXTERN_FUNC\(__imp__(\w+)\)', open(shared).read())
names = [n for n in names if not re.match(r'sub_|__(save|rest)(gpr|fpr|vmx)', n)]
done = set(re.findall(r'KFUNC\((\w+)\)', open(kernel, encoding='utf-8').read()))
with open(out, 'w', encoding='utf-8') as f:
    f.write('// Généré par gen_stubs.py : imports noyau pas encore implémentés\n#include "guest.h"\n\n')
    for n in names:
        if n not in done:
            f.write(f'PPC_FUNC(__imp__{n}) {{ LogImport("{n}", ctx); ctx.r3.u64 = 0; }}\n')
print(len(names), 'imports,', len(done & set(names)), 'implémentés')
