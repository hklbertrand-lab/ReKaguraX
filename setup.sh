#!/usr/bin/env bash
# Prépare les dépendances et le code généré d'Ikaruga.
# Prérequis : git, cmake, ninja, clang (llvm-mingw sous Windows), python ; le jeu extrait dans extracted/
#   python tools/stfs_extract.py "<paquet LIVE>" extracted
set -e
cd "$(dirname "$0")"
mkdir -p thirdparty build

clone() { [ -d "thirdparty/$1" ] || git clone --recursive $3 "$2" "thirdparty/$1"; }
clone XenonRecomp    https://github.com/hedge-dev/XenonRecomp
clone XenosRecomp    https://github.com/hedge-dev/XenosRecomp
clone Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers "--depth 1 --branch v1.4.304"
clone volk           https://github.com/zeux/volk "--depth 1 --branch 1.4.304"

# Patches locaux (instructions VMX ajoutées, adaptations aux shaders d'Ikaruga)
git -C thirdparty/XenonRecomp apply --check ../../patches/xenonrecomp-ikaruga.patch 2>/dev/null && git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-ikaruga.patch
git -C thirdparty/XenosRecomp apply --check ../../patches/xenosrecomp-ikaruga.patch 2>/dev/null && git -C thirdparty/XenosRecomp apply ../../patches/xenosrecomp-ikaruga.patch

FLAGS="-include cstdlib -include cstring"
cmake -S thirdparty/XenonRecomp -B build/XenonRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="$FLAGS"
cmake --build build/XenonRecomp
cmake -S thirdparty/XenosRecomp -B build/XenosRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENOS_RECOMP_DXIL=OFF \
    -DCMAKE_CXX_FLAGS="$FLAGS -include atomic -include algorithm -include execution -fexperimental-library"
cmake --build build/XenosRecomp

# 1) Code : tables de switch puis PPC -> C++
build/XenonRecomp/XenonAnalyse/XenonAnalyse extracted/default.xex config/ikaruga_switch_tables.toml
(cd config && ../build/XenonRecomp/XenonRecomp/XenonRecomp ikaruga.toml ../thirdparty/XenonRecomp/XenonUtils/ppc_context.h)
python runtime/gen_stubs.py ppc/ppc_recomp_shared.h runtime/kernel.cpp runtime/imports_stub.cpp

# 2) Shaders : image déchiffrée -> HLSL -> SPIR-V
build/xex_dump extracted/default.xex build/shaders/in/default.bin 2>/dev/null || true
(cd build/XenosRecomp/XenosRecomp && ./XenosRecomp ../../shaders/in ../../shaders/shader_cache.cpp ../../../thirdparty/XenosRecomp/XenosRecomp/shader_common.h)

# 3) Jeu
cmake -S . -B build/game -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/game --target ikaruga
