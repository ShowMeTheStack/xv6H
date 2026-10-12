#!/bin/bash
# A stock Linux 6.12.41 for the legacy-tier runs that need a kernel with neither the module nor
# the patches (runs on the build box). The source is extracted afresh from the tarball into its own tree
# (build.sh patches linux-6.12.41 in place), no patch is applied, and the kernel is configured from
# the release guest's own .config and built with the release guest's initramfs, whose machrev.ko does
# not load on a kernel without the patches' symbols. Output: ~/xv6h-linux/out-stock/Image-hf,
# with linux.img beside it, so that LXTAG=-stock selects it in tools/remote.sh.
set -ex
set -o pipefail
W=$HOME/xv6h-linux; V=6.12.41; S=$W/stock-$V; O=$W/obj-stock; OUT=$W/out-stock
cd $W
rm -rf $S && mkdir -p $S && tar -xf linux-$V.tar.xz -C $S --strip-components=1
mkdir -p $O $OUT
cp $W/obj-hf/.config $O/.config
sed -i "s|^CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE=\"$W/out-freeze21/initramfs-hf.cpio.gz\"|" $O/.config
make -s -C $S O=$O ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
time make -s -C $S O=$O ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j24 Image
cp $O/arch/riscv/boot/Image $OUT/Image-hf
cp $O/.config $OUT/Image-hf.config
cp $W/out-freeze21/linux.img $OUT/linux.img
# no patch's symbol may be in the image: it is the stock kernel
! grep -a -q "riscv_timer_revise\|virtio_mmio_relocate\|riscv_rebind_hart\|riscv_v_revise\|xv6h,vic" $OUT/Image-hf
md5sum $OUT/Image-hf | tee $OUT/BUILDSUM
echo STOCK_BUILD_DONE
