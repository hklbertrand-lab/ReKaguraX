#!/usr/bin/env python3
# Construit sans root un sysroot Debian 11 (glibc 2.31) pour que le kit Linux et le jeu produit tournent sur
# toute distribution récente (SteamOS compris), quelle que soit la glibc de la machine qui fabrique le kit.
# Usage : python3 make_sysroot.py <dossier>
import lzma, re, os, subprocess, sys, urllib.request

MIRROR = "https://deb.debian.org/debian"
DIST = "bullseye"
PKGS = """libc6 libc6-dev linux-libc-dev libgcc-10-dev libgcc-s1 libstdc++-10-dev libstdc++6
libx11-dev libx11-6 x11proto-dev libxext-dev libxrandr-dev libxrender-dev libxfixes-dev libxi-dev libxcursor-dev
libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libegl-dev libgl-dev libgles-dev
libasound2-dev libpulse-dev libpipewire-0.3-dev libspa-0.2-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev
libxcb1-dev libxau-dev libxdmcp-dev libibus-1.0-dev wayland-protocols
libpulse0 libasound2 libpipewire-0.3-0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0
libxext6 libxrandr2 libxi6 libxcursor1 libxfixes3 libxrender1 libxss1 libxtst6 libxcb1 libxau6 libxdmcp6
libudev1 libdbus-1-3 libegl1 libgl1 libgles2 libglvnd0 libdrm2 libgbm1 libibus-1.0-5""".split()

def main(root):
    os.makedirs(root, exist_ok=True)
    idx = {}
    for comp in ("main",):
        raw = urllib.request.urlopen(f"{MIRROR}/dists/{DIST}/{comp}/binary-amd64/Packages.xz").read()
        name = None
        for line in lzma.decompress(raw).decode().splitlines():
            if line.startswith("Package: "): name = line[9:]
            elif line.startswith("Filename: ") and name: idx.setdefault(name, line[10:])
    deb = os.path.join(root, "_deb"); os.makedirs(deb, exist_ok=True)
    for p in PKGS:
        if p not in idx: print("absent :", p); continue
        f = os.path.join(deb, os.path.basename(idx[p]))
        if not os.path.exists(f): urllib.request.urlretrieve(f"{MIRROR}/{idx[p]}", f)
        # data.tar.* du paquet .deb (archive ar) -> sysroot
        subprocess.run(f"bsdtar -xOf '{f}' 'data.tar.*' | bsdtar -xf - -C '{root}'", shell=True, check=True)
    # Liens absolus des .so de développement -> relatifs au sysroot
    for d, _, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            if os.path.islink(p) and os.readlink(p).startswith("/"):
                t = root + os.readlink(p); os.remove(p); os.symlink(os.path.relpath(t, d), p)
    # Scripts ld (libc.so, libpthread.so…) : chemins absolus -> =/… (relatifs au sysroot)
    for d in ("usr/lib/x86_64-linux-gnu",):
        for n in os.listdir(os.path.join(root, d)):
            p = os.path.join(root, d, n)
            if n.endswith(".so") and not os.path.islink(p) and os.path.getsize(p) < 4096:
                s = open(p, "rb").read()
                if b"GNU ld script" in s:
                    open(p, "wb").write(re.sub(rb"([ (])(/(?:usr/)?lib(?:64)?/)", rb"\1=\2", s))
    print("sysroot prêt :", root)

main(sys.argv[1])
