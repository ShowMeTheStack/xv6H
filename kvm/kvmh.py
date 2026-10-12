#!/usr/bin/env python3
# The KVM harness: a KVM host booted under QEMU, driven over its console.
# Guests run under lkvm in the background with their consoles in files; the host shell runs
# `lkvm revise` and reads the guests' logs. Used by every KVM test (t_*.py).
import os, re, subprocess, sys, time, select
K = os.path.expanduser("~/xv6h-kvm")

class Host:
    def __init__(self, cpus=4, mem="4G", aia="aplic-imsic,aia-guests=7", cpu=None, log="host.log", extra=()):
        cmd = ["qemu-system-riscv64", "-M", "virt,aia=" + aia, "-cpu", cpu or os.environ.get("HOSTCPU", "rva23s64"),
               "-smp", str(cpus), "-m", mem, "-nographic",
               "-kernel", os.environ.get("HOSTKERNEL", K + "/build-host/arch/riscv/boot/Image"), "-initrd", os.environ.get("HOSTINITRD", K + "/host-initramfs.cpio.gz"),
               "-append", "console=ttyS0 earlycon"] + list(extra)
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.buf = b""; self.log = open(log, "wb")
        if not self.expect("kvmhost# ", 600):
            raise RuntimeError("host did not boot")
    def expect(self, pat, t):
        end = time.time() + t; rx = re.compile(pat.encode()) if isinstance(pat, str) else pat
        while time.time() < end:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]; return m
            r, _, _ = select.select([self.p.stdout], [], [], 1)
            if r:
                d = os.read(self.p.stdout.fileno(), 65536)
                if not d: break
                self.buf += d; self.log.write(d); self.log.flush()
        return None
    def run(self, cmd, t=120):
        """a host shell command; returns its output"""
        self.buf = b""
        sep = " " if cmd.rstrip().endswith("&") else "; "
        self.p.stdin.write((cmd + sep + "echo __DONE__$?\n").encode()); self.p.stdin.flush()
        m = self.expect(r"__DONE__(\d+)", t)
        out = self.buf
        return (m.string[:m.start()] if m else self.buf).decode(errors="replace")
    def close(self):
        try: self.p.kill()
        except Exception: pass
        self.log.close()

def guest_cmd(name, mem=256, cpus=2, extra=""):
    """the guest's console input is a FIFO held open by a sleeper, so tests can type into it.
    virtio over MMIO (kvmtool's riscv default is PCI): the transport xv6H's guests use, and the
    one whose windows F15 moves"""
    return ("rm -f /tmp/{n}.in; mkfifo /tmp/{n}.in; (sleep 1000000 > /tmp/{n}.in &); "
            "lkvm run --name {n} -k /guest/Image -m {m} -c {c} --console serial --virtio-transport mmio -d /guest/linux.img "
            "-p 'console=ttyS0 earlycon' {x} < /tmp/{n}.in > /tmp/{n}.log 2>&1 &").format(n=name, m=mem, c=cpus, x=extra)

def guest_send(h, name, cmd):
    """type a line into the guest's shell"""
    h.run("echo '%s' > /tmp/%s.in" % (cmd.replace("'", "'\\''"), name), 30)

def guest_do(h, name, cmd, t=60, mark=None):
    """run a command in the guest and return what it printed between two markers"""
    import random
    tag = mark or ("M%06d" % random.randrange(10**6))
    before = len(h.run("cat /tmp/%s.log" % name, 30))
    guest_send(h, name, "echo %s_A; %s; echo %s_B" % (tag, cmd, tag))
    end = time.time() + t
    while time.time() < end:
        out = h.run("cat /tmp/%s.log" % name, 30).replace("\r", "")
        i = out.rfind(tag + "_A\n"); j = out.rfind(tag + "_B\n")
        if i >= 0 and j > i:
            return out[i + len(tag) + 3:j]
        time.sleep(1)
    return None

def wait_log(h, name, pat, t):
    end = time.time() + t
    while time.time() < end:
        out = h.run("cat /tmp/%s.log" % name, 30)
        if re.search(pat, out):
            return out
        time.sleep(2)
    return None
