#!/bin/bash
# A separate KVM host with patch 4 (the IMSIC's file backing switched
# under a running guest), so the runs using ~/xv6h-kvm are not disturbed. Everything goes in
# ~/xv6h-kvm-d5: the kernel tree (patches 1-3 as in ~/xv6h-kvm, plus 4), its build, kvmtool with
# the imsicfile command, and the host's initramfs.
set -ex
set -o pipefail
K=$HOME/xv6h-kvm; D=$HOME/xv6h-kvm-d5
mkdir -p $D
[ -d $D/linux-6.18.54 ] || cp -a $K/linux-6.18.54 $D/
KT=$D/linux-6.18.54 python3 $K/apply-kvm-patches.py
mkdir -p $D/build-host
[ -f $D/build-host/.config ] || cp $K/build-host/.config $D/build-host/.config
cd $D/linux-6.18.54
make -s O=$D/build-host ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
time make -s O=$D/build-host ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j16 Image
ls -la $D/build-host/arch/riscv/boot/Image
(cd $D/linux-6.18.54 && diff -u $K/linux-6.18.54/arch/riscv/kvm/aia_imsic.c arch/riscv/kvm/aia_imsic.c; \
  diff -u $K/linux-6.18.54/arch/riscv/kvm/aia_device.c arch/riscv/kvm/aia_device.c; \
  diff -u $K/linux-6.18.54/arch/riscv/include/asm/kvm_aia.h arch/riscv/include/asm/kvm_aia.h; \
  diff -u $K/linux-6.18.54/arch/riscv/include/uapi/asm/kvm.h arch/riscv/include/uapi/asm/kvm.h) > $D/patch4.diff || true
# kvmtool: a copy with the imsicfile command
rm -rf $D/kvmtool && cp -a $K/kvmtool $D/kvmtool
cp $K/kvmtool-src-d5/revise-facts.c $D/kvmtool/riscv/revise-facts.c
# libfdt's library and headers in one directory: kvmtool adds -I $(LIBFDT_DIR) itself, and a
# CFLAGS given on the command line would replace its own include paths
mkdir -p $D/fdt && cp $K/sysroot/lib/libfdt.a $K/sysroot/include/*.h $D/fdt/
cd $D/kvmtool && rm -f lkvm-static && make -s ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- LIBFDT_DIR=$D/fdt \
  -j16 lkvm-static 2>&1 | tail -5
[ "$(strings $D/kvmtool/lkvm-static | grep -c "imsicfile hw|sw")" -gt 0 ]
ls -la $D/kvmtool/lkvm-static
# the host's initramfs, as mkhost.sh, with this lkvm
R=$D/hostroot; rm -rf $R; mkdir -p $R
cp -a $HOME/xv6h-linux/bb-hf/_install/. $R/
mkdir -p $R/proc $R/sys $R/dev $R/tmp $R/guest $R/etc $R/mnt $R/root $R/var/run
cp $D/kvmtool/lkvm-static $R/bin/lkvm
cp $HOME/xv6h-linux/out/Image-hf $R/guest/Image
cp $HOME/xv6h-linux/out/linux.img $R/guest/linux.img
cp $K/hostroot/init $R/init
(cd $R && find . | cpio -o -H newc --quiet | gzip -1 > $D/host-initramfs.cpio.gz)
ls -la $D/host-initramfs.cpio.gz
echo D5_BUILD_DONE
