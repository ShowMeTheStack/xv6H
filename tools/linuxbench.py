#!/usr/bin/env python3
#
# run schedbench on Linux, on the same QEMU, so that this kernel's
# numbers have something to be next to.
#
# this runs on the build box (it needs the kernel, the musl cross
# compiler and qemu): tools/linuxbench.sh drives it from the workstation.
#
#   linuxbench.py [-t secs] [-c cpus] [test ...]
#
import os, re, subprocess, sys, time, tempfile, shutil

WORK = os.path.expanduser("~/xv6h-linux")
OUT = WORK + "/out"
MUSL = WORK + "/musl/bin/riscv64-linux-musl-gcc"

secs, cpus, tests = "3", "3", []
a = sys.argv[1:]
while a:
    if a[0] == "-t":
        secs = a[1]; a = a[2:]
    elif a[0] == "-c":
        cpus = a[1]; a = a[2:]
    else:
        tests.append(a[0]); a = a[1:]
if not tests:
    tests = ["msg", "wake", "fair", "forkexec", "ctx"]

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "user", "schedbench.c")
tmp = tempfile.mkdtemp(prefix="linuxbench")
try:
    exe = tmp + "/schedbench"
    subprocess.run([MUSL, "-O2", "-static", "-o", exe, src], check=True)

    # a small disk with nothing on it but the benchmark
    root = tmp + "/root"
    os.mkdir(root)
    shutil.copy(exe, root + "/schedbench")
    img = tmp + "/bench.img"
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-d", root, img, "8M"],
                   check=True)

    cmd = ["qemu-system-riscv64", "-machine", "virt", "-cpu", "rv64",
           "-smp", cpus, "-m", "1024", "-nographic", "-no-reboot",
           "-kernel", OUT + "/Image-hf",
           "-drive", "file=%s,if=none,format=raw,id=d0" % img,
           "-device", "virtio-blk-device,drive=d0",
           "-append", "console=ttyS0 loglevel=3"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0)

    buf = b""
    def wait(pat, t):
        global buf
        end = time.time() + t
        r = re.compile(pat.encode())
        while time.time() < end:
            c = p.stdout.read(1)
            if not c:
                break
            buf += c
            if r.search(buf[-4096:]):
                return True
        raise SystemExit("linuxbench: timed out waiting for %r; tail:\n%s"
                         % (pat, buf[-2000:].decode("utf-8", "replace")))

    def send(s):
        p.stdin.write((s + "\n").encode())
        p.stdin.flush()

    wait(r"xv6h-linux: ready", 120)
    wait(r"linux# |/ # |# $", 60)
    send("stty -echo 2>/dev/null; mkdir -p /mnt && mount -t ext2 /dev/vda /mnt && echo MOUNTED")
    wait(r"MOUNTED", 30)
    mark = len(buf)
    send("/mnt/schedbench -t %s %s; echo BENCHDONE" % (secs, " ".join(tests)))
    wait(r"BENCHDONE", 600)
    out = buf[mark:].decode("utf-8", "replace")
    for line in out.splitlines():
        line = line.strip()
        if line and not line.startswith("/mnt/schedbench") and line != "BENCHDONE":
            print(line)
    send("poweroff -f")
    time.sleep(2)
    p.kill()
finally:
    shutil.rmtree(tmp, ignore_errors=True)
