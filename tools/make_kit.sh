#!/bin/sh
# Assemble le kit communautaire dist/ReKaguraX-kit : outils, compilateur, runtime et sources d'Ikaruga,
# SANS aucune donnée ni code du jeu (pas d'extracted/, de ppc/ ni de shaders traduits). L'utilisateur fournit son
# paquet LIVE ; rkx-setup le vérifie, l'extrait et recompile tout sur sa machine.
# Usage (depuis Ikaruga/, après setup.sh) : sh tools/make_kit.sh
set -e
cd "$(dirname "$0")/.."
TC=/e/tools/llvm-mingw-20261020-ucrt-x86_64
RT=../Xbox360Runtime
TP=thirdparty
KIT=dist/ReKaguraX-kit
export PATH="$TC/bin:$PATH"

rm -rf "$KIT"
mkdir -p "$KIT/bin" "$KIT/toolchain/bin" "$KIT/toolchain/lib" "$KIT/src/include" "$KIT/src/lib"

# --- Assistant
clang++ -std=c++20 -O2 -I"$TP/SDL3/include" tools/rkx_setup/rkx_setup.cpp -L"$TP/SDL3/lib" -lSDL3 -mwindows -o "$KIT/rkx-setup.exe"
cp "$TP/SDL3/bin/SDL3.dll" "$TC/x86_64-w64-mingw32/bin/libc++.dll" "$TC/x86_64-w64-mingw32/bin/libunwind.dll" "$KIT/"

# --- Outils de recompilation
cp build/XenonRecomp/XenonRecomp/XenonRecomp.exe build/XenosRecomp/XenosRecomp/XenosRecomp.exe \
   build/XenosRecomp/XenosRecomp/*.dll build/xex_dump.exe "$KIT/bin/"
cp "$(command -v ninja)" "$KIT/bin/ninja.exe" 2>/dev/null || cp "$(command -v ninja.exe)" "$KIT/bin/"
cp "$TC/x86_64-w64-mingw32/bin/libc++.dll" "$TC/x86_64-w64-mingw32/bin/libunwind.dll" "$KIT/bin/"

# --- Compilateur (x86_64 seulement)
for f in clang.exe clang++.exe clang-23.exe ld.lld.exe lld.exe libLLVM-23.dll libclang-cpp.dll libc++.dll libunwind.dll \
         x86_64-w64-windows-gnu.cfg x86_64-pc-windows-gnu.cfg mingw32-common.cfg; do
    [ -f "$TC/bin/$f" ] && cp "$TC/bin/$f" "$KIT/toolchain/bin/"
done
cp -r "$TC/include" "$KIT/toolchain/"
cp -r "$TC/lib/clang" "$KIT/toolchain/lib/"
cp -r "$TC/x86_64-w64-mingw32" "$KIT/toolchain/"

# --- Sources (aucune issue du jeu) : runtime, partie Ikaruga, config XenonRecomp
cp -r "$RT/src" "$KIT/src/runtime"
cp -r game "$KIT/src/game"
mkdir -p "$KIT/src/config"
cp config/ikaruga.toml config/ikaruga_switch_tables.toml "$KIT/src/config/"

# --- En-têtes et bibliothèques tierces
I="$KIT/src/include"
cp "$TP/XenonRecomp/XenonUtils/ppc_context.h" "$I/"
mkdir -p "$I/XenonUtils" && (cd "$TP/XenonRecomp/XenonUtils" && find . \( -name "*.h" -o -name "*.hpp" \) -exec cp --parents {} "$OLDPWD/$I/XenonUtils/" \;)
cp -r "$TP/XenonRecomp/thirdparty/simde" "$I/simde"
cp -r "$TP/Vulkan-Headers/include/vulkan" "$TP/Vulkan-Headers/include/vk_video" "$I/"
cp -r "$TP/SDL3/include/SDL3" "$I/"
cp "$TP/volk/volk.h" "$TP/volk/volk.c" "$I/"
X="$TP/XenosRecomp/thirdparty"
cp "$X/smol-v/source/smolv.h" "$X/smol-v/source/smolv.cpp" "$X/xxHash/xxhash.h" "$X/zstd/lib/zstd.h" "$X/zstd/lib/zstd_errors.h" "$I/"
cp "$TP/XenosRecomp/XenosRecomp/shader_common.h" "$I/"
mkdir -p "$I/ffmpeg" && cp -r "$TP/ffmpeg-include/libavcodec" "$TP/ffmpeg-include/libavutil" "$I/ffmpeg/"
L="$KIT/src/lib"
cp build/game/thirdparty/XenonRecomp/XenonUtils/libXenonUtils.a build/game/thirdparty/XenonRecomp/thirdparty/disasm/libdisasm.a \
   build/game/zstd/lib/libzstd.a "$TP/ffmpeg-build/libavcodec/libavcodec.a" "$TP/ffmpeg-build/libavutil/libavutil.a" \
   "$TP/SDL3/lib/libSDL3.dll.a" "$L/"

# Le jeu a besoin de SDL3.dll et du runtime C++ à côté de ikaruga.exe : déjà copiés à la racine du kit
cp README-communaute.txt "$KIT/LISEZMOI.txt" 2>/dev/null || true

# Garde-fou : rien du jeu dans le kit
if [ -n "$(find "$KIT" -name 'default.xex' -o -name 'ppc_recomp*' -o -name 'shader_cache.cpp' -o -name '*.xpr' | head -1)" ]; then
    echo "ERREUR : données du jeu dans le kit" >&2; exit 1
fi
du -sh "$KIT"
echo "kit : $(du -sh "$KIT" | cut -f1)"; "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" //Q tools/installer_kit.iss && ls -la dist/ReKaguraX-communaute-setup.exe
