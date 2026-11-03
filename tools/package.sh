#!/bin/sh
# Prépare dist/ReKaguraX (exe + DLL + données du jeu) puis compile l'installateur Inno Setup.
# Usage (depuis Ikaruga/) : sh tools/package.sh
# L'installateur contient les données extraites du jeu : il est pour un usage personnel, ne pas le diffuser.
set -e
cd "$(dirname "$0")/.."
TOOLCHAIN=/e/tools/llvm-mingw-20261020-ucrt-x86_64
ISCC="/c/Program Files (x86)/Inno Setup 6/ISCC.exe"
OUT=dist/ReKaguraX

rm -rf "$OUT"
mkdir -p "$OUT"
cp build/game/ikaruga.exe build/game/SDL3.dll "$OUT/"
cp "$TOOLCHAIN/x86_64-w64-mingw32/bin/libc++.dll" "$TOOLCHAIN/x86_64-w64-mingw32/bin/libunwind.dll" "$OUT/"
cp -r extracted "$OUT/extracted"

# Exe en sous-système fenêtré (pas de console noire au lancement) ; le point d'entrée reste main()
python - "$OUT/ikaruga.exe" <<'EOF'
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
pe = struct.unpack_from('<I', b, 0x3C)[0]
assert b[pe:pe + 4] == b'PE\0\0'
struct.pack_into('<H', b, pe + 24 + 68, 2)   # IMAGE_SUBSYSTEM_WINDOWS_GUI
open(p, 'wb').write(b)
EOF

"$ISCC" //Q tools/installer.iss
ls -la dist/*.exe
