#!/bin/sh
#
# build fsbench for the Linux guest and the ext4 disk it runs on.
#
# the comparison the benchmark is for -- xv6H against Linux on the same
# QEMU and the same kind of disk -- only means something if both sides
# see the same file system. so this makes the guest's disk with the same
# mkext4 that makes xv6H's: 4096-byte blocks, a 1024-block journal, and
# the same size.
#
# it runs on a Linux box with a riscv64 cross compiler, because that is
# where the guest's userland comes from. the result is bench/bench.img,
# which goes into the host image as a file the guest boots with:
#
#   sh bench/build-linux.sh
#   make fs.img HOSTFS_MB=192 HOSTFILES="linux/Image linux/linux.img bench/bench.img"
#
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

CC=${CC:-riscv64-linux-gnu-gcc}
command -v "$CC" >/dev/null || { echo "no $CC; this runs on the build box"; exit 1; }
[ -x mkfs/mkext4 ] || make -s mkfs/mkext4

echo "building fsbench for riscv64 linux"
"$CC" -O2 -static -Wall -o bench/fsbench-linux user/fsbench.c

echo "building bench/bench.img"
rm -f bench/bench.img
mkfs/mkext4 -b 8192 -j 1024 -L bench bench/bench.img fsbench:bench/fsbench-linux
mkfs/mkext4 -l bench/bench.img | head -4
