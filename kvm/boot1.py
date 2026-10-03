#!/usr/bin/env python3
# A.0 smoke test (runs on the build box): boot the KVM host under QEMU (AIA with guest interrupt files,
# H, Sstc, V, Zicboz), start the unchanged xv6H Linux guest under lkvm, and check it comes up.
import os, subprocess, sys, time, select
K = os.path.expanduser("~/xv6h-kvm")
irqchip = sys.argv[1] if len(sys.argv) > 1 else ""
cmd = ["qemu-system-riscv64", "-M", "virt,aia=aplic-imsic,aia-guests=7", "-cpu", os.environ.get("HOSTCPU", "rva23s64"),
       "-smp", "4", "-m", "4G", "-nographic", "-kernel", K + "/build-host/arch/riscv/boot/Image",
       "-initrd", K + "/host-initramfs.cpio.gz", "-append", "console=ttyS0 earlycon"]
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
buf = b""; log = open(K + "/boot1.log", "wb")
def expect(pat, t):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 1)
        if r:
            d = os.read(p.stdout.fileno(), 65536)
            if not d: break
            buf += d; log.write(d); log.flush()
            if pat.encode() in buf:
                i = buf.index(pat.encode()) + len(pat); buf = buf[i:]; return True
    return False
def send(s): p.stdin.write(s.encode()); p.stdin.flush()
try:
    t0 = time.time()
    ok = expect("kvmhost# ", 600); print("host shell:", ok, "%.0fs" % (time.time() - t0)); sys.stdout.flush()
    if not ok: sys.exit(1)
    send("cat /proc/cpuinfo | grep -m1 isa; dmesg | grep -i -m3 kvm\n"); expect("kvmhost# ", 60)
    extra = (" --irqchip=%s" % irqchip) if irqchip else ""
    t1 = time.time()
    send("lkvm run -k /guest/Image -m 256 -c 2 --console serial -d /guest/linux.img -p 'console=ttyS0 earlycon'%s\n" % extra)
    ok = expect("linux# ", 1800); print("guest shell:", ok, "%.0fs" % (time.time() - t1)); sys.stdout.flush()
    if ok:
        send("cat /proc/interrupts | head -12; dmesg | grep -i machrev | head -5; uname -a\n"); expect("linux# ", 120)
finally:
    p.kill()
    log.close()
    print(open(K + "/boot1.log", errors="replace").read()[-6000:])
