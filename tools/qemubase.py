#!/usr/bin/env python3
# qemubase (L19): what today's hypervisor does on the same move. The same Linux guest
# (the Image xv6H's tests boot, with its initramfs) under bare QEMU, migrated by QEMU's
# own live migration between two QEMU instances on this box: once between machines of
# the same shape (downtime and total time from QMP's query-migrate), then between
# machines that differ in one thing each -- the interrupt controller, the hart count,
# the memory size, a device's slot, a device present or absent, the vector unit -- and
# what QEMU says. One row per shape difference: succeeded (downtime, total) or refused
# (the message).
#   python3 tools/qemubase.py [rounds]
import json, os, re, socket, subprocess, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
IMAGE = os.path.expanduser("~/xv6h-linux/out/Image-hf")
BASE = ["qemu-system-riscv64", "-nographic", "-bios", "default", "-kernel", IMAGE,
        "-append", "console=ttyS0 earlycon=sbi", "-global", "virtio-mmio.force-legacy=false"]
CPU = "rv64,v=true,vlen=128,zicbom=true,zicboz=true,cboz_blocksize=64"

def machine(aia="none", smp=2, mem=256, cpu=CPU, blkbus=0, rng=False):
    m = ["-machine", "virt,aia=%s" % aia + (",aia-guests=2" if aia != "none" else ""), "-cpu", cpu,
         "-m", str(mem), "-smp", str(smp),
         "-drive", "file=/dev/null,if=none,format=raw,id=x0,readonly=on",
         "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.%d" % blkbus,
         "-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1"]
    if rng:
        m += ["-device", "virtio-rng-device,bus=virtio-mmio-bus.3"]
    return m

SHAPES = [
    ("same shape", {}),
    ("interrupt controller: aia=none -> aplic-imsic (F1)", {"aia": "aplic-imsic"}),
    ("hart count: 2 -> 3 (F7)", {"smp": 3}),
    ("hart count: 2 -> 1 (F8)", {"smp": 1}),
    ("memory: 256 -> 512 MB (F13)", {"mem": 512}),
    ("a device's slot: block on bus 0 -> bus 2 (F15)", {"blkbus": 2}),
    ("a device added: virtio-rng (F16)", {"rng": True}),
    ("vector unit absent on the destination (F10)", {"cpu": "rv64,v=false,zicbom=true,zicboz=true,cboz_blocksize=64"}),
    ("cache block: 64 -> 128 (F21)", {"cpu": "rv64,v=true,vlen=128,zicbom=true,zicboz=true,cboz_blocksize=128"}),
]

ROW = [0]
class Q:
    def __init__(self, name, opts, incoming=None):
        self.name = "%s%d" % (name, ROW[0])      # a log of its own per row: a later row's tail is its own
        self.qmp = "/tmp/qb-%s-%d.qmp" % (name, os.getpid())
        try: os.unlink(self.qmp)
        except OSError: pass
        cmd = BASE + opts + ["-qmp", "unix:%s,server,nowait" % self.qmp]
        if incoming:
            cmd += ["-incoming", incoming]
        self.log = open("qemu-%s.log" % self.name, "wb")
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = None
    def connect(self):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX); s.connect(self.qmp); self.sock = s; break
            except OSError:
                time.sleep(0.1)
        self.f = self.sock.makefile("rwb", buffering=0)
        self.f.readline()                                    # the greeting
        self.cmd("qmp_capabilities")
    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            r = json.loads(line)
            if "return" in r or "error" in r:
                return r
    def wait_ready(self, t=180):
        t0 = time.time()
        while time.time() - t0 < t:
            data = open("qemu-%s.log" % self.name, "rb").read()
            if b"xv6h-linux: ready" in data or b"Run /init as init process" in data:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(1)
        return False
    def tail(self):
        try:
            return open("qemu-%s.log" % self.name, "rb").read()[-800:].decode(errors="replace")
        except Exception:
            return ""
    def close(self):
        try:
            self.proc.kill(); self.proc.wait(5)
        except Exception: pass
        try: os.unlink(self.qmp)
        except OSError: pass

def one(label, diff):
    ROW[0] += 1
    port = 9500 + (os.getpid() % 100)
    src = Q("src", machine())
    dst = Q("dst", machine(**diff), incoming="tcp:127.0.0.1:%d" % port)
    try:
        src.connect(); dst.connect()
        if not src.wait_ready():
            return ("source did not boot", None, None, src.tail())
        time.sleep(3)
        r = src.cmd("migrate", uri="tcp:127.0.0.1:%d" % port)
        if "error" in r:
            return ("refused at once", None, None, r["error"].get("desc", str(r)))
        for _ in range(600):
            st = src.cmd("query-migrate")["return"]
            if st.get("status") in ("completed", "failed", "cancelled"):
                break
            time.sleep(0.2)
        status = st.get("status")
        if status == "completed":
            # the destination's guest must answer for it to count: its serial keeps printing
            time.sleep(3)
            alive = dst.proc.poll() is None
            dtail = dst.tail()
            failed_load = "error while loading" in dtail.lower() or "load of migration failed" in dtail.lower()
            if alive and not failed_load:
                # and what does the guest believe now? nproc, memory, the ISA string, the vector
                # unit tried, the cache block the kernel uses -- against what the destination has
                try:
                    dst.proc.stdin.write(b"\necho _probe_; nproc; grep MemTotal /proc/meminfo; grep -m1 isa /proc/cpuinfo; vecuser use 1; echo rc=$?; ls /dev/hwrng 2>&1; echo _probed_\n")
                    dst.proc.stdin.flush()
                    for _ in range(30):
                        time.sleep(1)
                        d = open("qemu-%s.log" % dst.name, "rb").read()
                        if b"_probed_" in d:
                            break
                    seg = d[d.rfind(b"_probe_"):].decode(errors="replace")
                    guest = " | ".join(l.strip() for l in seg.splitlines() if l.strip() and "_probe" not in l)[:400]
                except Exception as e:
                    guest = "guest probe failed: %s" % str(e)[:80]
                return ("succeeded", st.get("downtime"), st.get("total-time"), guest)
            return ("completed on the source, refused on the destination", st.get("downtime"), st.get("total-time"),
                    "\n".join(l for l in dtail.splitlines() if "migrat" in l.lower() or "error" in l.lower())[-400:])
        desc = st.get("error-desc", "")
        dtail = dst.tail()
        return ("refused (%s)" % status, None, None,
                (desc + " | " + "\n".join(l for l in dtail.splitlines() if "migrat" in l.lower() or "error" in l.lower()))[-500:])
    finally:
        src.close(); dst.close()

print("QEMU %s" % subprocess.run(["qemu-system-riscv64", "--version"], capture_output=True, text=True).stdout.split("\n")[0])
for label, diff in SHAPES:
    rows = []
    for i in range(N if not diff else 1):
        rows.append(one(label, diff))
    if rows[0][0] == "succeeded":
        dts = sorted(r[1] for r in rows if r[1] is not None); tts = sorted(r[2] for r in rows if r[2] is not None)
        print("%-52s succeeded (n=%d): downtime median %s ms [%s-%s], total median %s ms; the guest then: %s" % (
            label, len(rows), dts[len(dts)//2], dts[0], dts[-1], tts[len(tts)//2], rows[0][3]))
    else:
        print("%-52s %s: %s" % (label, rows[0][0], rows[0][3].strip().replace("\n", " | ")[:300]))
    sys.stdout.flush()
