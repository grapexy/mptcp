#!/bin/sh
set -e
set -x

# Apply patches
patch -p1 < kernel-patches/5.4/bbr2.patch

MPTCP_VERSION=$(git symbolic-ref --short HEAD | grep -E -o "[0-9\.]+")
make clean
make x86_64_mptcp_defconfig
make -j$(nproc) bindeb-pkg LOCALVERSION="-mptcp" KDEB_PKGVERSION=$(make kernelversion)-1
