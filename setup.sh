#!/usr/bin/env bash
# Prépare les dépendances et le code généré d'Ikaruga.
# Prérequis : git, cmake, ninja, clang (llvm-mingw sous Windows), python ; le jeu extrait dans extracted/
#   python tools/stfs_extract.py "<paquet LIVE>" extracted
set -e
cd "$(dirname "$0")"
mkdir -p thirdparty build
RT="$(cd ../Xbox360Runtime && pwd)"   # runtime commun (dossier frère)

clone() { [ -d "thirdparty/$1" ] || git clone --recursive $3 "$2" "thirdparty/$1"; }
clone XenonRecomp    https://github.com/hedge-dev/XenonRecomp
clone XenosRecomp    https://github.com/hedge-dev/XenosRecomp
clone Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers "--depth 1 --branch v1.4.304"
clone volk           https://github.com/zeux/volk "--depth 1 --branch 1.4.304"

# Patches locaux (instructions VMX ajoutées, adaptations aux shaders d'Ikaruga)
git -C thirdparty/XenonRecomp apply --check "$RT/patches/xenonrecomp.patch" 2>/dev/null && git -C thirdparty/XenonRecomp apply "$RT/patches/xenonrecomp.patch"
git -C thirdparty/XenosRecomp apply --check "$RT/patches/xenosrecomp.patch" 2>/dev/null && git -C thirdparty/XenosRecomp apply "$RT/patches/xenosrecomp.patch"

FLAGS="-include cstdlib -include cstring"
cmake -S thirdparty/XenonRecomp -B build/XenonRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="$FLAGS"
cmake --build build/XenonRecomp
cmake -S thirdparty/XenosRecomp -B build/XenosRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENOS_RECOMP_DXIL=OFF \
    -DCMAKE_CXX_FLAGS="$FLAGS -include atomic -include algorithm -include execution -fexperimental-library"
cmake --build build/XenosRecomp

# 1) Code : tables de switch puis PPC -> C++
build/XenonRecomp/XenonAnalyse/XenonAnalyse extracted/default.xex config/ikaruga_switch_tables.toml
(cd config && ../build/XenonRecomp/XenonRecomp/XenonRecomp ikaruga.toml ../thirdparty/XenonRecomp/XenonUtils/ppc_context.h)
python "$RT/tools/gen_stubs.py" ppc/ppc_recomp_shared.h "$RT/src/kernel.cpp" game/imports_stub.cpp

# 2) Shaders : image déchiffrée -> HLSL -> SPIR-V
build/xex_dump extracted/default.xex build/shaders/in/default.bin 2>/dev/null || true
(cd build/XenosRecomp/XenosRecomp && ./XenosRecomp ../../shaders/in ../../shaders/shader_cache.cpp ../../../thirdparty/XenosRecomp/XenosRecomp/shader_common.h)

# 3) FFmpeg réduit au décodeur XMA (statique, sans assembleur). Sous Windows, mingw32-make (fourni par
#    llvm-mingw) ne comprend pas les chemins /e/... : on les réécrit dans les fichiers générés.
if [ ! -f thirdparty/ffmpeg-build/libavcodec/libavcodec.a ]; then
    [ -d thirdparty/ffmpeg-7.1 ] || curl -sSL https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz | tar -xJ -C thirdparty
    mkdir -p thirdparty/ffmpeg-build/tmp
    case "$(uname -s)" in MINGW*|MSYS*) TARGET="--target-os=mingw32 --cc=x86_64-w64-mingw32-clang"; MAKE=mingw32-make ;; *) TARGET="--cc=clang"; MAKE=make ;; esac
    (cd thirdparty/ffmpeg-build && TMPDIR=$PWD/tmp ../ffmpeg-7.1/configure $TARGET --arch=x86_64 --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm \
        --enable-static --disable-shared --disable-everything --disable-programs --disable-doc --disable-network --disable-avdevice \
        --disable-avformat --disable-swscale --disable-swresample --disable-avfilter --disable-postproc --disable-asm \
        --disable-autodetect --disable-debug --enable-decoder=xma1 --enable-decoder=xma2)
    case "$(uname -s)" in MINGW*|MSYS*) sed -i "s#$(cd thirdparty && pwd)#$(cd thirdparty && pwd -W)#g" thirdparty/ffmpeg-build/Makefile thirdparty/ffmpeg-build/ffbuild/config.mak ;; esac
    (cd thirdparty/ffmpeg-build && $MAKE -j4 SHELL=sh)
fi
# En-têtes publics seuls : la racine des sources contient un fichier VERSION qui masque <version>
mkdir -p thirdparty/ffmpeg-include/libavcodec thirdparty/ffmpeg-include/libavutil
cp thirdparty/ffmpeg-7.1/libavcodec/*.h thirdparty/ffmpeg-include/libavcodec/
cp thirdparty/ffmpeg-7.1/libavutil/*.h thirdparty/ffmpeg-build/libavutil/avconfig.h thirdparty/ffmpeg-build/libavutil/ffversion.h thirdparty/ffmpeg-include/libavutil/

# 4) Jeu
cmake -S . -B build/game -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/game --target ikaruga
