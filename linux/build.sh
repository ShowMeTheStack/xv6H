#!/bin/bash
#
# build a minimal RISC-V Linux kernel with an embedded static BusyBox
# initramfs, plus an ext2 disk image, for use as an xv6H guest.
# runs on a Linux box with gcc-riscv64-linux-gnu (tested: Ubuntu 26.04).
#
#   ./build.sh            builds everything into ~/xv6h-linux/out/
#
set -euo pipefail
WORK=${WORK:-$HOME/xv6h-linux}
OUT=$WORK/out
KVER=${KVER:-6.12.41}
BBVER=${BBVER:-1.37.0}
CROSS=riscv64-linux-gnu-
J=$(nproc)
mkdir -p "$WORK" "$OUT"
cd "$WORK"

log() { echo "=== $(date +%H:%M:%S) $*"; }

# ---- sources ----
if [ ! -d linux-$KVER ]; then
  log "fetching linux $KVER"
  curl -fsSL -o linux-$KVER.tar.xz https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz
  tar xf linux-$KVER.tar.xz
fi
if [ ! -d busybox-$BBVER ]; then
  log "fetching busybox $BBVER"
  curl -fsSL -o busybox-$BBVER.tar.bz2 https://busybox.net/downloads/busybox-$BBVER.tar.bz2
  tar xf busybox-$BBVER.tar.bz2
fi

# ---- the xv6H patches (linux/patches/*.patch): small, listed in PATCHES.md ----
if [ -d $WORK/src/patches ]; then
  # the files a patch touches come back pristine from the tarball first, so a
  # patch that changed since the last build applies to the original, not to
  # its own previous version
  for p in $WORK/src/patches/*.patch; do
    [ -f "$p" ] || continue
    for f in $(grep -E '^\+\+\+ b/' "$p" | sed 's|^+++ b/||; s|\t.*||'); do
      tar xf linux-$KVER.tar.xz linux-$KVER/$f 2>/dev/null || log "no pristine $f in the tarball"
    done
  done
  for p in $WORK/src/patches/*.patch; do
    [ -f "$p" ] || continue
    if patch -d linux-$KVER -p1 -N --dry-run -s < "$p" >/dev/null 2>&1; then
      log "applying $(basename $p)"
      patch -d linux-$KVER -p1 -N -s < "$p"
    else
      log "$(basename $p): already applied (or does not apply)"
    fi
  done
fi
# ---- busybox (static) ----
# Ubuntu's riscv64 toolchain and glibc target the RVA23 profile, whose
# vector instructions the xv6H guest CPU (rv64gc) does not have. BusyBox
# is therefore built with a musl cross toolchain at plain rv64gc.
if [ ! -x musl/bin/riscv64-linux-musl-gcc ]; then
  log "fetching musl cross toolchain"
  curl -fsSL -o musl.tgz https://musl.cc/riscv64-linux-musl-cross.tgz
  rm -rf musl; mkdir musl; tar xzf musl.tgz -C musl --strip-components=1
fi
MUSL=$WORK/musl/bin/riscv64-linux-musl-
build_busybox() { # $1 = dir suffix, $2 = arch flags
  local d=bb-$1
  if [ -x $d/_install/bin/busybox ]; then return; fi
  log "building busybox ($1)"
  rm -rf $d; cp -r busybox-$BBVER $d
  ( cd $d
    make -s ARCH=riscv CROSS_COMPILE=$MUSL defconfig >/dev/null
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
    sed -i 's/^CONFIG_SHA1_HWACCEL=y/# CONFIG_SHA1_HWACCEL is not set/; s/^CONFIG_SHA256_HWACCEL=y/# CONFIG_SHA256_HWACCEL is not set/' .config
    make -s -j$J ARCH=riscv CROSS_COMPILE=$MUSL EXTRA_CFLAGS="$2" EXTRA_LDFLAGS="$2" install >/dev/null
  )
  local nv=$($WORK/musl/bin/riscv64-linux-musl-objdump -d $d/_install/bin/busybox | grep -cE "vset|vle|vse" || true)
  log "busybox ($1): $(du -h $d/_install/bin/busybox | cut -f1), vector instructions: $nv"
}
build_busybox hf "-march=rv64gc -mabi=lp64d"
build_busybox sf "-march=rv64imac -mabi=lp64" || log "soft-float busybox failed, continuing"

# ---- initramfs ----
make_initramfs() { # $1 = busybox dir, $2 = output cpio.gz
  local root=$WORK/rootfs-$1
  rm -rf $root; mkdir -p $root
  cp -a bb-$1/_install/. $root/
  mkdir -p $root/lib/modules
  [ -f $OUT/machrev-$1.ko ] && cp $OUT/machrev-$1.ko $root/lib/modules/machrev.ko
  # zerocheck: fresh pages are zero (F21's consumer, seen from userland)
  if [ -f $WORK/src/zerocheck.c ]; then
    ${MUSL}gcc -O2 -static -march=rv64gc -mabi=lp64d -o $root/bin/zerocheck $WORK/src/zerocheck.c || log "zerocheck did not build"
  fi
  mkdir -p $root/proc $root/sys $root/dev $root/etc $root/mnt $root/tmp $root/root
  cat > $root/init <<'INIT'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
# /dev/console did not exist when the kernel started us: attach to it now.
exec </dev/console >/dev/console 2>&1
# the kernel configured eth0 from ip=... on the command line (dhcp or
# static); its name server is in /proc/net/pnp, in resolv.conf format.
ln -sf /proc/net/pnp /etc/resolv.conf
echo "xv6h-linux: init running on $(uname -m), $(nproc) cpu(s)"
# the machine-description revision driver (machrev/): with it loaded this
# guest speaks the revision protocol and the host treats it as participating
# (machrev=off on the command line leaves it out: the legacy tier, same image)
if [ -f /lib/modules/machrev.ko ] && ! grep -q machrev=off /proc/cmdline; then
  insmod /lib/modules/machrev.ko
fi
echo "xv6h-linux: ready"
exec /sbin/init
INIT
  chmod +x $root/init
  cat > $root/etc/inittab <<'TAB'
::sysinit:/bin/true
ttyS0::respawn:/bin/sh -l
ttyS0::ctrlaltdel:/sbin/reboot
TAB
  echo "marker-in-initramfs" > $root/etc/marker
  # the TLS client built by linux/build-https.sh, if present.
  [ -x $OUT/https ] && cp $OUT/https $root/bin/https || true
  echo "export PS1='linux# '" > $root/etc/profile
  ( cd $root && find . | cpio -o -H newc --quiet | gzip -9 > $2 )
}
make_initramfs hf $OUT/initramfs-hf.cpio.gz
[ -x bb-sf/_install/bin/busybox ] && make_initramfs sf $OUT/initramfs-sf.cpio.gz || true

# ---- kernel ----
build_kernel() { # $1 = name, $2 = initramfs, $3 = extra config fragment lines
  local d=obj-$1
  if [ -f $OUT/Image-$1 ] && [ -z "${REBUILD:-}" ]; then return; fi
  log "building kernel $1"
  mkdir -p $d
  cat > $d/fragment.config <<CFG
CONFIG_INITRAMFS_SOURCE="$2"
CONFIG_INITRAMFS_COMPRESSION_GZIP=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_SERIAL_OF_PLATFORM=y
CONFIG_SERIAL_EARLYCON=y
CONFIG_RISCV_SBI_V01=y
CONFIG_SERIAL_EARLYCON_RISCV_SBI=y
CONFIG_HVC_RISCV_SBI=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_BLK=y
CONFIG_EXT2_FS=y
CONFIG_EXT4_FS=y
CONFIG_SIFIVE_PLIC=y
CONFIG_RISCV_APLIC=y
CONFIG_RISCV_IMSIC=y
CONFIG_RISCV_ISA_SVNAPOT=n
CONFIG_RISCV_BOOT_SPINWAIT=n
CONFIG_SMP=y
CONFIG_NR_CPUS=8
CONFIG_RISCV_SBI_CPUIDLE=n
CONFIG_CPU_IDLE=n
CONFIG_PCI=n
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IP_PNP=y
CONFIG_IP_PNP_DHCP=y
CONFIG_UNIX=y
CONFIG_PACKET=y
CONFIG_VIRTIO_NET=y
CONFIG_RTC_CLASS=y
CONFIG_RTC_DRV_GOLDFISH=y
CONFIG_RTC_HCTOSYS=y
CONFIG_RTC_HCTOSYS_DEVICE="rtc0"
CONFIG_RTC_SYSTOHC=n
CONFIG_IPV6=n
CONFIG_WIRELESS=n
CONFIG_BT=n
CONFIG_NETFILTER=n
CONFIG_USB_SUPPORT=n
CONFIG_DRM=n
CONFIG_SOUND=n
CONFIG_INPUT=n
CONFIG_HID_SUPPORT=n
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_MEMORY_HOTPLUG=y
CONFIG_MEMORY_HOTREMOVE=y
CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE=y
CONFIG_HOTPLUG_CPU=y
CONFIG_VIRTIO_BALLOON=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_DEBUG_INFO_NONE=y
CONFIG_KASAN=n
CONFIG_FTRACE=n
CONFIG_VIRTIO_PCI=n
CONFIG_BLK_DEV_INITRD=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_TMPFS=y
CONFIG_PRINTK_TIME=y
$3
CFG
  ( cd linux-$KVER
    make -s O=../$d ARCH=riscv CROSS_COMPILE=$CROSS defconfig >/dev/null
    ./scripts/kconfig/merge_config.sh -O ../$d -m ../$d/.config ../$d/fragment.config >/dev/null
    make -s O=../$d ARCH=riscv CROSS_COMPILE=$CROSS olddefconfig >/dev/null
    make -s O=../$d ARCH=riscv CROSS_COMPILE=$CROSS -j$J Image modules 2>&1 | grep -E "error|Error" -A3 || true
    [ -f ../$d/arch/riscv/boot/Image ] && [ ../$d/arch/riscv/boot/Image -nt ../$d/.config ] || { log "KERNEL $1 DID NOT BUILD"; exit 1; }
  )
  # the revision driver, against this kernel; then the initramfs again with
  # it inside, and the image again around that (only the initramfs relinks)
  if [ -d $WORK/src/machrev ]; then
    log "building machrev.ko ($1)"
    rm -rf $d/machrev; cp -r $WORK/src/machrev $d/machrev
    ( cd linux-$KVER && make -s O=../$d ARCH=riscv CROSS_COMPILE=$CROSS M=../$d/machrev modules 2>&1 | grep -E "error|warning" || true )
    if [ -f $d/machrev/machrev.ko ]; then
      cp $d/machrev/machrev.ko $OUT/machrev-$1.ko
      make_initramfs $1 $2
      ( cd linux-$KVER && make -s O=../$d ARCH=riscv CROSS_COMPILE=$CROSS -j$J Image 2>&1 | grep -E "error|Error" -A3 || true )
      [ $d/arch/riscv/boot/Image -nt $d/machrev/machrev.ko ] || { log "KERNEL $1 DID NOT RELINK WITH THE MODULE"; exit 1; }
    else
      log "machrev.ko ($1) did not build"
    fi
  fi
  cp $d/arch/riscv/boot/Image $OUT/Image-$1
  grep -E "^CONFIG_(FPU|RISCV_ISA_C|SMP|RISCV_APLIC|RISCV_IMSIC|SIFIVE_PLIC|VIRTIO_MMIO|RISCV_SBI_V01)=" $d/.config > $OUT/Image-$1.config
  log "kernel $1 done: $(du -h $OUT/Image-$1 | cut -f1)"
}
build_kernel hf $OUT/initramfs-hf.cpio.gz "CONFIG_FPU=y"
if [ -f $OUT/initramfs-sf.cpio.gz ]; then
  build_kernel nofpu $OUT/initramfs-sf.cpio.gz "CONFIG_FPU=n"
fi

# ---- ext2 disk image with a marker and the busybox tree ----
if [ ! -f $OUT/linux.img ]; then
  log "building ext2 disk image"
  rm -rf diskroot; mkdir -p diskroot
  echo "marker-on-virtio-disk" > diskroot/marker
  mkdir -p diskroot/bin
  cp bb-hf/_install/bin/busybox diskroot/bin/
  dd if=/dev/zero of=$OUT/linux.img bs=1M count=16 status=none
  mke2fs -q -t ext2 -d diskroot -L xv6hdisk $OUT/linux.img
fi

log "artifacts:"
ls -la $OUT
