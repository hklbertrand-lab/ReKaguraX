#!/usr/bin/env bash
# Build Linux portable d'Ikaruga et des outils du kit (à lancer depuis Ikaruga/, extracted/ présent).
# Usage : RKX_TC=<llvm> RKX_SYSROOT=<sysroot> RKX_LDLIBS=<libxml2/ICU> bash tools/linux/build_linux.sh
set -e
cd "$(dirname "$0")/../.."
export RKX_PREFIX="$PWD/build/prefix"
. tools/linux/env.sh

# SDL3 (même version que sous Windows), compilée contre le sysroot : X11, Wayland, PipeWire/Pulse/ALSA chargés à l'exécution
if [ ! -f "$RKX_PREFIX/lib/libSDL3.so" ]; then
    [ -d thirdparty/SDL3-src ] || git clone --depth 1 --branch release-3.4.8 https://github.com/libsdl-org/SDL thirdparty/SDL3-src
    cmake -S thirdparty/SDL3-src -B build/sdl3 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$RKX_PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_WAYLAND_LIBDECOR=OFF -DSDL_X11_XSCRNSAVER=OFF -DSDL_LIBURING=OFF -DSDL_KMSDRM=OFF
    cmake --build build/sdl3
    cmake --install build/sdl3
fi

bash setup.sh
# rkx-setup (assistant) : cherche SDL3 et le compilateur à côté de lui
clang++ -std=c++20 -O2 -I"$RKX_PREFIX/include" tools/rkx_setup/rkx_setup.cpp -L"$RKX_PREFIX/lib" -lSDL3 \
    -Wl,-rpath,'$ORIGIN/lib' -o build/rkx-setup
echo "glibc requise :"; for f in build/game/ikaruga build/rkx-setup build/xex_dump build/XenonRecomp/XenonRecomp/XenonRecomp build/XenosRecomp/XenosRecomp/XenosRecomp; do
    printf '  %-50s %s\n' "$f" "$(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)"; done
