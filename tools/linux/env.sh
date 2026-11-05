# Environnement de compilation portable (sourcé) : clang officiel + sysroot Debian 11 + libc++ statique.
# Les binaires produits ne dépendent que de glibc >= 2.31 : ils tournent sur toute distribution récente.
# Variables attendues : RKX_TC (dossier LLVM, bin/clang), RKX_SYSROOT, RKX_LDLIBS (libxml2/ICU pour ld.lld)
: "${RKX_TC:?}" "${RKX_SYSROOT:?}"
RKX_WRAP="${RKX_WRAP:-$(dirname "$RKX_TC")/wrap}"
mkdir -p "$RKX_WRAP"
cat > "$RKX_WRAP/clang" <<EOF
#!/bin/sh
exec "$RKX_TC/bin/clang" --sysroot="$RKX_SYSROOT" -fuse-ld=lld -Wno-unused-command-line-argument "\$@"
EOF
cat > "$RKX_WRAP/clang++" <<EOF
#!/bin/sh
for a in "\$@"; do case "\$a" in -c|-E|-S|-M|-MM) exec "$RKX_TC/bin/clang++" --sysroot="$RKX_SYSROOT" -stdlib=libc++ -Wno-unused-command-line-argument "\$@";; esac; done
exec "$RKX_TC/bin/clang++" --sysroot="$RKX_SYSROOT" -stdlib=libc++ -fuse-ld=lld -Wno-unused-command-line-argument "\$@" \
    -nostdlib++ -Wl,-Bstatic -lc++ -lc++abi -lunwind -Wl,-Bdynamic -pthread -ldl
EOF
chmod +x "$RKX_WRAP/clang" "$RKX_WRAP/clang++"
for t in llvm-ar llvm-ranlib llvm-nm; do ln -sf "$RKX_TC/bin/$t" "$RKX_WRAP/$t"; done
export PATH="$RKX_WRAP:$RKX_TC/bin:$PATH"
[ -n "$RKX_LDLIBS" ] && export LD_LIBRARY_PATH="$RKX_LDLIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CC=clang CXX=clang++ AR=llvm-ar RANLIB=llvm-ranlib
export PKG_CONFIG_SYSROOT_DIR="$RKX_SYSROOT"
export PKG_CONFIG_LIBDIR="$RKX_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$RKX_SYSROOT/usr/share/pkgconfig:$RKX_PREFIX/lib/pkgconfig"
RKX_TOOLCHAIN="$RKX_WRAP/toolchain.cmake"
cat > "$RKX_TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSROOT "$RKX_SYSROOT")
set(CMAKE_C_COMPILER "$RKX_WRAP/clang")
set(CMAKE_CXX_COMPILER "$RKX_WRAP/clang++")
set(CMAKE_AR "$RKX_TC/bin/llvm-ar")
set(CMAKE_RANLIB "$RKX_TC/bin/llvm-ranlib")
set(CMAKE_FIND_ROOT_PATH "$RKX_PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "$RKX_PREFIX")
EOF
export CMAKE_TOOLCHAIN_FILE="$RKX_TOOLCHAIN"
