#!/bin/bash
# A.0 (runs on the build box): the KVM host's initramfs: busybox, lkvm, and the xv6H Linux guest exactly
# as the xv6H runs use it (Image-hf with machrev.ko in its own initramfs, and the disk linux.img).
set -e
K=$HOME/xv6h-kvm; R=$K/hostroot
case "$K" in $HOME/*) ;; *) echo "refusing: not on the build box ($K)"; exit 1;; esac
rm -rf $R; mkdir -p $R
cp -a $HOME/xv6h-linux/bb-hf/_install/. $R/
mkdir -p $R/proc $R/sys $R/dev $R/tmp $R/guest $R/etc $R/mnt $R/root $R/var/run
cp $K/kvmtool/lkvm-static $R/bin/lkvm
cp $HOME/xv6h-linux/out/Image-hf $R/guest/Image
cp $HOME/xv6h-linux/out/linux.img $R/guest/linux.img
cat > $R/init <<'INIT'
#!/bin/sh
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
mount -t tmpfs tmpfs /tmp; mkdir -p /dev/pts; mount -t devpts devpts /dev/pts
exec </dev/console >/dev/console 2>&1
echo "kvm-host: $(uname -r), $(nproc) cpus, /dev/kvm $( [ -e /dev/kvm ] && echo present || echo MISSING )"
export PS1="kvmhost# " HOME=/root
exec /bin/sh
INIT
chmod +x $R/init
(cd $R && find . | cpio -o -H newc --quiet | gzip -1 > $K/host-initramfs.cpio.gz)
ls -la $K/host-initramfs.cpio.gz
