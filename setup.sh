#!/usr/bin/env bash
# Récupère et compile XenonRecomp (nécessite git, cmake, ninja, clang / clang-cl)
set -e
cd "$(dirname "$0")"
mkdir -p thirdparty
[ -d thirdparty/XenonRecomp ] || git clone --recursive https://github.com/hedge-dev/XenonRecomp thirdparty/XenonRecomp
cmake -S thirdparty/XenonRecomp -B build/XenonRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/XenonRecomp
# 1) Analyse : tables de switch
build/XenonRecomp/XenonAnalyse/XenonAnalyse extracted/default.xex config/ikaruga_switch_tables.toml
# 2) Recompilation PPC -> C++
cd config && ../build/XenonRecomp/XenonRecomp/XenonRecomp ikaruga.toml ../thirdparty/XenonRecomp/XenonUtils/ppc_context.h
