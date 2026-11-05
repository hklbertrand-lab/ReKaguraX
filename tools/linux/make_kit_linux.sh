#!/usr/bin/env bash
# Assemble le kit communautaire Linux dist/ReKaguraX-kit-linux (et son archive .tar.xz), SANS aucune donnée ni code
# du jeu. À lancer depuis Ikaruga/ après tools/linux/build_linux.sh, avec les mêmes RKX_TC / RKX_SYSROOT / RKX_LDLIBS.
# Le compilateur du kit est le clang officiel + des fichiers .cfg (sysroot Debian 11, libc++ statique) : le jeu
# recompilé chez l'utilisateur tourne sur toute distribution à glibc >= 2.31 (SteamOS compris).
set -e
cd "$(dirname "$0")/../.."
: "${RKX_TC:?}" "${RKX_SYSROOT:?}" "${RKX_LDLIBS:?}"
export RKX_PREFIX="$PWD/build/prefix"
. tools/linux/env.sh
RT="$(cd ../Xbox360Runtime 2>/dev/null && pwd || echo "$PWD/Xbox360Runtime")"
TP=thirdparty
KIT=dist/ReKaguraX-kit-linux
rm -rf "$KIT"
mkdir -p "$KIT/bin" "$KIT/toolchain/bin" "$KIT/toolchain/lib" "$KIT/toolchain/include" "$KIT/toolchain/ldlibs" "$KIT/src/include" "$KIT/src/lib"

# --- Assistant + SDL3 (à côté des exécutables : rpath $ORIGIN)
clang++ -std=c++20 -O2 -I"$RKX_PREFIX/include" tools/rkx_setup/rkx_setup.cpp -L"$RKX_PREFIX/lib" -lSDL3 -Wl,-rpath,'$ORIGIN' -o "$KIT/rkx-setup"
cp -L "$RKX_PREFIX/lib/libSDL3.so.0" "$KIT/"
ln -s ../../libSDL3.so.0 "$KIT/src/lib/libSDL3.so"

# --- Outils de recompilation
cp build/XenonRecomp/XenonRecomp/XenonRecomp build/XenosRecomp/XenosRecomp/XenosRecomp build/xex_dump "$KIT/bin/"
cp -L "$TP/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$KIT/bin/"
cp "$(command -v ninja)" "$KIT/bin/ninja"

# --- Compilateur : clang + lld officiels, en-têtes libc++, bibliothèques statiques, sysroot
V=$(ls "$RKX_TC/lib/clang")
cp "$RKX_TC/bin/clang-$V" "$RKX_TC/bin/lld" "$KIT/toolchain/bin/"
for l in clang clang++; do ln -s "clang-$V" "$KIT/toolchain/bin/$l"; done
ln -s lld "$KIT/toolchain/bin/ld.lld"
mkdir -p "$KIT/toolchain/lib/clang/$V"
cp -r "$RKX_TC/lib/clang/$V/include" "$KIT/toolchain/lib/clang/$V/"
cp -r "$RKX_TC/include/c++" "$KIT/toolchain/include/"
mkdir -p "$KIT/toolchain/include/x86_64-unknown-linux-gnu" "$KIT/toolchain/lib/x86_64-unknown-linux-gnu"
cp -r "$RKX_TC/include/x86_64-unknown-linux-gnu/c++" "$KIT/toolchain/include/x86_64-unknown-linux-gnu/"
cp "$RKX_TC/lib/x86_64-unknown-linux-gnu/"{libc++.a,libc++abi.a,libunwind.a} "$KIT/toolchain/lib/x86_64-unknown-linux-gnu/"
cp -L "$RKX_LDLIBS/libxml2.so.2" "$RKX_LDLIBS/libicuuc.so.67" "$RKX_LDLIBS/libicudata.so.67" "$KIT/toolchain/ldlibs/"
# Sysroot réduit : en-têtes, objets de démarrage et bibliothèques (sans les .deb, docs ni pages de manuel)
cp -a "$RKX_SYSROOT" "$KIT/toolchain/sysroot"
rm -rf "$KIT/toolchain/sysroot/_deb" "$KIT/toolchain/sysroot/usr/share/doc" "$KIT/toolchain/sysroot/usr/share/man" \
       "$KIT/toolchain/sysroot/usr/share/locale" "$KIT/toolchain/sysroot/usr/share/lintian" "$KIT/toolchain/sysroot/usr/bin" \
       "$KIT/toolchain/sysroot/usr/sbin" "$KIT/toolchain/sysroot/sbin" "$KIT/toolchain/sysroot/bin" "$KIT/toolchain/sysroot/etc" "$KIT/toolchain/sysroot/var"
# Options par défaut (chemins relatifs au .cfg) ; les lignes « $ » ne servent qu'à l'édition des liens
for d in clang clang++; do
    { echo "--sysroot=<CFGDIR>/../sysroot"; echo "-fuse-ld=lld"; echo "-Wno-unused-command-line-argument"; } > "$KIT/toolchain/bin/$d.cfg"
done
cat >> "$KIT/toolchain/bin/clang++.cfg" <<'EOF'
-stdlib=libc++
$-nostdlib++
$-Wl,-Bstatic
$-lc++
$-lc++abi
$-lunwind
$-Wl,-Bdynamic
EOF

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
cp -r "$RKX_PREFIX/include/SDL3" "$I/"
cp "$TP/volk/volk.h" "$TP/volk/volk.c" "$I/"
X="$TP/XenosRecomp/thirdparty"
cp "$X/smol-v/source/smolv.h" "$X/smol-v/source/smolv.cpp" "$X/xxHash/xxhash.h" "$X/zstd/lib/zstd.h" "$X/zstd/lib/zstd_errors.h" "$I/"
cp "$TP/XenosRecomp/XenosRecomp/shader_common.h" "$I/"
mkdir -p "$I/ffmpeg" && cp -r "$TP/ffmpeg-include/libavcodec" "$TP/ffmpeg-include/libavutil" "$I/ffmpeg/"
L="$KIT/src/lib"
cp build/game/thirdparty/XenonRecomp/XenonUtils/libXenonUtils.a build/game/thirdparty/XenonRecomp/thirdparty/disasm/libdisasm.a \
   build/game/zstd/lib/libzstd.a "$TP/ffmpeg-build/libavcodec/libavcodec.a" "$TP/ffmpeg-build/libavutil/libavutil.a" "$L/"

cp README-communaute.txt "$KIT/LISEZMOI.txt" 2>/dev/null || true

# Garde-fou : rien du jeu dans le kit
if [ -n "$(find "$KIT" -name 'default.xex' -o -name 'ppc_recomp*' -o -name 'shader_cache.cpp' -o -name '*.xpr' | head -1)" ]; then
    echo "ERREUR : données du jeu dans le kit" >&2; exit 1
fi
echo "kit : $(du -sh "$KIT" | cut -f1)"
tar -C dist -cJf dist/ReKaguraX-linux.tar.xz ReKaguraX-kit-linux --transform 's/^ReKaguraX-kit-linux/ReKaguraX/'
ls -la dist/ReKaguraX-linux.tar.xz
