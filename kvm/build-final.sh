#!/bin/bash
# The final KVM stack (runs on the build box), built from this repository's kvm/ alone into ~/xv6h-kvm-final,
# so that every KVM result in the paper comes from one build: Linux 6.18.54 from the pristine tree
# with KVM patches 1-4 (apply-kvm-patches.py), kvmtool upstream f67bc0b with this repository's
# changes (apply-kvmtool.py's edits, on the files in kvm/kvmtool), and the frozen Linux guest
# (~/xv6h-linux/out-freeze21). The repository's kvm/ is copied to ~/xv6h-kvm-final/src first.
#   kvm/build-final.sh            (on the build box, after: rsync -a kvm/ buildbox:xv6h-kvm-final/src/)
set -ex
set -o pipefail
K=$HOME/xv6h-kvm; F=$HOME/xv6h-kvm-final; S=$F/src
[ -d $S ] || { echo "copy the repository's kvm/ to $S first"; exit 1; }
# the kernel: pristine tree, patches 1-4
rm -rf $F/linux-6.18.54 && cp -a $K/linux-6.18.54.orig $F/linux-6.18.54
KT=$F/linux-6.18.54 python3 $S/apply-kvm-patches.py
(cd $F && diff -ru $K/linux-6.18.54.orig linux-6.18.54 > $F/kvm-patches.diff) || true
mkdir -p $F/build-host && cp $K/build-host/.config $F/build-host/.config
cd $F/linux-6.18.54
make -s O=$F/build-host ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
time make -s O=$F/build-host ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j24 Image
ls -la $F/build-host/arch/riscv/boot/Image
# kvmtool: upstream f67bc0b, then this repository's files and edits
rm -rf $F/kvmtool && cp -a $K/kvmtool $F/kvmtool
git -C $F/kvmtool checkout -q -- . && git -C $F/kvmtool clean -qfd
[ "$(git -C $F/kvmtool rev-parse --short=7 HEAD)" = f67bc0b ]
mkdir -p $F/kvmtool-src && cp -a $S/kvmtool/. $F/kvmtool-src/
sed -e "s|~/xv6h-kvm/kvmtool-src|$F/kvmtool-src|" -e "s|~/xv6h-kvm/kvmtool\"|$F/kvmtool\"|" $S/apply-kvmtool.py > $F/apply-kvmtool-final.py
grep -q "$F/kvmtool\"" $F/apply-kvmtool-final.py
python3 $F/apply-kvmtool-final.py
(cd $F/kvmtool && git diff > $F/kvmtool.diff; git status --porcelain >> $F/kvmtool.diff)
mkdir -p $F/fdt && cp $K/sysroot/lib/libfdt.a $K/sysroot/include/*.h $F/fdt/
cd $F/kvmtool && rm -f lkvm-static && make -s ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- LIBFDT_DIR=$F/fdt \
  -j24 lkvm-static 2>&1 | tail -5
[ "$(strings $F/kvmtool/lkvm-static | grep -c "imsicfile hw|sw")" -gt 0 ]
ls -la $F/kvmtool/lkvm-static
# the host's initramfs, as mkhost.sh, with this lkvm and the frozen guest
R=$F/hostroot; rm -rf $R; mkdir -p $R
cp -a $HOME/xv6h-linux/bb-hf/_install/. $R/
mkdir -p $R/proc $R/sys $R/dev $R/tmp $R/guest $R/etc $R/mnt $R/root $R/var/run
cp $F/kvmtool/lkvm-static $R/bin/lkvm
cp $HOME/xv6h-linux/out-freeze21/Image-hf $R/guest/Image
cp $HOME/xv6h-linux/out/linux.img $R/guest/linux.img
cp $K/hostroot/init $R/init
(cd $R && find . | cpio -o -H newc --quiet | gzip -1 > $F/host-initramfs.cpio.gz)
md5sum $F/build-host/arch/riscv/boot/Image $F/kvmtool/lkvm-static $R/guest/Image $F/host-initramfs.cpio.gz | tee $F/BUILDSUMS
echo FINAL_KVM_BUILD_DONE
