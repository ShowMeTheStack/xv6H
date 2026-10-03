#!/usr/bin/env python3
# B.2 on the board: xv6H on Rocket + H on the ZedBoard, from nothing, over JTAG and the USB-UART.
#   tools/remote.sh script zedb tools/zedboard.py [steps]     steps: host (default), guest, linux
#
# What it does, in order:
#   - the images: the kernel as a raw image with its .bss in it (DDR is not zeroed the way
#     QEMU's RAM is), the board's device tree, the 90 MB root (one xv6 guest disk, the Linux
#     guest's kernel and disk);
#   - the console: /dev/ttyACM0 (the board's USB-UART, PS UART1) opened first, so nothing the
#     machine says is missed;
#   - the load (fpga/zedboard/load.tcl, xsdb in the Vivado container): the PS reset and set up,
#     the PL programmed, DDR loaded with Rocket held in reset, the ARM's UART bridge started,
#     Rocket let go;
#   - then the same steps as tools/zedqemu.py: the host's boot, an xv6 guest, a Linux guest.
# Rocket's 0x8xxx_xxxx is DDR 0x1xxx_xxxx (rocket_wrap.v).
import os, re, subprocess, sys, time, termios
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE)
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G, test_hyp_linux as L

FPGA = os.path.expanduser("~/xv6h-fpga")
if len(sys.argv) > 1 and sys.argv[1] == "--ila-summary":    # tools/zedboard.py --ila-summary FILE.csv
    pass
args = [a for a in sys.argv[1:] if "=" not in a]
opts = dict(a.split("=", 1) for a in sys.argv[1:] if "=" in a)
BUILD = os.path.expanduser(opts.get("build", FPGA + "/zed-XV6HZedSoCConfig"))   # build=~/xv6h-fpga/zed-...
steps = args or ["host", "guest", "linux"]
if opts.get("uartdebug"):
    os.environ["ZED_BOOTARGS"] = "uartdebug"

def sh(c):
    print("+", c); sys.stdout.flush()
    subprocess.run(c, shell=True, check=True)

# ---- the images, under ~/xv6h-fpga (the container sees that tree only)
IMG = FPGA + "/zed-images"
os.makedirs(IMG, exist_ok=True)
# a save run leaves the Linux guest's disk out: the saved machine needs the room (it ran out at 24 MB)
hostfiles = "linux/Image fpga/zedboard/lx fpga/zedboard/lxv" if ("save" in steps or "restore" in steps) else "linux/Image linux/linux.img fpga/zedboard/lx fpga/zedboard/lxv"
sh("rm -f fs.img && make -s fs.img HOSTFS_MB=90 GUESTDISKS=1 HOSTFILES='%s' && cp fs.img %s/fs.img" % (hostfiles, IMG))
sh("riscv64-unknown-elf-objcopy -O binary --set-section-flags .bss=alloc,load,contents kernel/kernel %s/kernel.bin" % IMG)
sh("python3 fpga/zedboard/mkdts.py board > %s/zed.dts && dtc -q -I dts -O dtb -o %s/zed.dtb %s/zed.dts" % (IMG, IMG, IMG))
if "restore" in steps:
    # B.4, QEMU -> board: the machine saved on a QEMU host (tools/b4_depart.py) into the board's root
    sh("debugfs -w -R 'write %s/q.vst /q.vst' %s/fs.img" % (IMG, IMG))
sh("make -s -C fpga/zedboard/arm && cp fpga/zedboard/arm/bridge.elf %s/" % IMG)
sh("cd %s && rm -rf xsa && mkdir xsa && cd xsa && unzip -q -o %s/xv6hzed.xsa && ls" % (IMG, BUILD))
ps7 = subprocess.run("find %s/xsa -name ps7_init.tcl | head -1" % IMG, shell=True, capture_output=True, text=True).stdout.strip()
assert ps7, "no ps7_init.tcl in the xsa"
sh("cp %s %s/ps7_init.tcl && cp %s/xv6hzed.bit %s/" % (ps7, IMG, BUILD, IMG))
for f in ["kernel.bin", "zed.dtb", "fs.img"]:
    print(f, os.path.getsize(os.path.join(IMG, f)))

# ---- the console, opened before the machine starts
class Tty:
    def __init__(self, path, baud=termios.B115200):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
        a[4] = a[5] = baud
        a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
    def fileno(self): return self.fd
    def write(self, b):
        # the bridge copies byte by byte through two 64-byte FIFOs: pace the typing
        for i in range(0, len(b), 8):
            os.write(self.fd, b[i:i + 8]); time.sleep(0.03)
    def flush(self): pass
class P:
    def __init__(self, t): self.stdout = t; self.stdin = t
    def poll(self): return None
    def kill(self): pass
    def wait(self, t=None): pass

tty = Tty("/dev/ttyACM0")
q = object.__new__(T.QEMU)
q.cmd = "zedboard"; q.log = open("zedb-console.log", "w")
q.proc = P(tty); q.buf = b""; q.consumed = 0; q.gone = False

# ---- the load
t0 = time.time()
load = ("docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb -v {F}:{F} kkrizka/vivado:2020.1 bash -lc "
        "\"source /opt/Xilinx/Vivado/2020.1/settings64.sh && xsdb {F}/zedboard/load.tcl {I} {I}/bridge.elf "
        "{I}/kernel.bin 0x10000000 {I}/zed.dtb 0x1fe00000 {I}/fs.img 0x1a000000\"").format(F=FPGA, I=IMG)
if opts.get("at"):
    os.environ["ZED_ILA_AT"] = opts["at"]      # at=0x1xxxxxxx: the ILA triggers on Rocket fetching that DDR line
if opts.get("ila"):
    # the same load with the ILA armed on Rocket's reset (fpga/zedboard/debug.sh); ZED_RUNNING is
    # then the ILA's "written" line
    sh("cp fpga/zedboard/load.tcl fpga/zedboard/release.tcl fpga/zedboard/ila.tcl fpga/zedboard/debug.sh %s/zedboard/" % FPGA)
    load = "bash %s/zedboard/debug.sh %s %s && echo ZED_RUNNING" % (FPGA, IMG, BUILD)
print("+", load); sys.stdout.flush()
lp = subprocess.Popen(load, shell=True, stdout=open("zedb-load.log", "w"), stderr=subprocess.STDOUT)
def ila_summary(path):
    """what Rocket did in the 4096 cycles from its reset: reads and their answers, device writes"""
    import csv
    try:
        rows = list(csv.reader(open(path)))
    except OSError as e:
        return "no ILA capture: %s" % e
    hdr = rows[0]; data = rows[2:] if len(rows) > 2 and rows[1][0].startswith("Radix") else rows[1:]
    # the probe's own name, its last component without the bit range ("RVALID" is in "ARVALID")
    base = lambda h: h.split("/")[-1].split("[")[0]
    col = lambda key: next((i for i, h in enumerate(hdr) if base(h) == key), None)
    names = {"rstgate_Res": "rstgate_Res", "ARVALID": "rocket_m_mem_axi_ARVALID", "ARREADY": "rocket_m_mem_axi_ARREADY",
             "ARADDR": "rocket_m_mem_axi_ARADDR", "RVALID": "rocket_m_mem_axi_RVALID", "RREADY": "rocket_m_mem_axi_RREADY",
             "RRESP": "rocket_m_mem_axi_RRESP", "mmio_axi_AWVALID": "rocket_m_mmio_axi_AWVALID",
             "mmio_axi_AWREADY": "rocket_m_mmio_axi_AWREADY", "mmio_axi_AWADDR": "rocket_m_mmio_axi_AWADDR",
             "mmio_axi_WDATA": "rocket_m_mmio_axi_WDATA", "mmio_axi_WVALID": "rocket_m_mmio_axi_WVALID",
             "mmio_axi_BRESP": "rocket_m_mmio_axi_BRESP", "mmio_axi_BVALID": "rocket_m_mmio_axi_BVALID",
             "AWVALID": "rocket_m_mem_axi_AWVALID", "BRESP": "rocket_m_mem_axi_BRESP", "BVALID": "rocket_m_mem_axi_BVALID"}
    c = {k: col(n) for k, n in names.items()}
    def v(r, k, base=16):
        i = c.get(k)
        return int(r[i], base) if i is not None and r[i] not in ("", "X") else 0
    out = ["ILA: %d samples; probes missing: %s" % (len(data), [k for k, i in c.items() if i is None])]
    reads = [(n, v(r, "ARADDR")) for n, r in enumerate(data) if v(r, "ARVALID") and v(r, "ARREADY")]
    resps = [(n, v(r, "RRESP")) for n, r in enumerate(data) if v(r, "RVALID") and v(r, "RREADY")]
    mw = [(n, v(r, "mmio_axi_AWADDR"), v(r, "mmio_axi_WDATA")) for n, r in enumerate(data) if v(r, "mmio_axi_WVALID")]
    rst = [n for n, r in enumerate(data) if v(r, "rstgate_Res")]
    out.append("reset released at sample %s" % (rst[0] if rst else "never"))
    out.append("memory reads accepted: %d; first: %s" % (len(reads), ", ".join("%d:0x%08x" % x for x in reads[:12])))
    out.append("read responses: %d; RRESP values %s" % (len(resps), sorted(set(x[1] for x in resps))))
    out.append("device writes: %d; first: %s" % (len(mw), ", ".join("%d:0x%x=%x" % x for x in mw[:12])))
    chars = "".join(chr(w & 0xff) for _, a, w in mw if (a & 0xfff) == 0 and 32 <= (w & 0xff) < 127)
    out.append("UART bytes in the window: %r" % chars)
    wr = [n for n, r in enumerate(data) if v(r, "AWVALID")]
    out.append("memory writes started: %d; write responses not OKAY: %d" % (len(wr), sum(1 for r in data if v(r, "BVALID") and v(r, "BRESP"))))
    return "\n".join(out)

ok = {}
def step(name, f):
    t = time.time()
    try:
        f(); ok[name] = "ok %.1fs" % (time.time() - t)
    except Exception as e:
        ok[name] = "FAILED %.1fs: %s" % (time.time() - t, str(e)[-1500:])
        raise
    finally:
        print("STEP", name, ok[name]); sys.stdout.flush()
try:
    def loaded():
        while lp.poll() is None:
            q._pump(1.0)
        assert lp.returncode == 0, open("zedb-load.log").read()[-2000:]
        assert "ZED_RUNNING" in open("zedb-load.log").read(), open("zedb-load.log").read()[-2000:]
    step("load", loaded)
    print(open("zedb-load.log").read()[-1500:])
    if opts.get("ila"):
        print(ila_summary(BUILD + "/ila.csv"))
    step("bridge", lambda: q.expect(r"zed: bridge up", 30))
    step("host boot", lambda: (q.expect(r"xv6 kernel is booting", 60),
                               q.expect(r"hyp: (hypervisor extension present|no hypervisor extension)", 600),
                               q.expect(r"init: starting sh", 900), q.expect(r"\$ ", 120)))
    for c in ["cat /proc/cpuinfo", "dmesg | grep -i -E 'blk|plat|hyp|kalloc'", "sleep 5; echo slept"]:
        m = q.mark(); q.cmd_(c, timeout=300); print(q.output_since(m))
    if "guest" in steps:
        def xv6_guest():
            # G.start_guest, with a 40 MHz core's patience for the prompt
            q.send("vmm start -n g1 -c 1 -m 32\n")
            q.expect(r"vm (\d+)[^:]*: (\d+) vcpu", 120)
            q.expect(r"xv6 kernel is booting", 1800)
            q.expect(r"init: starting sh", 1800)
            q.expect(r"\$ ", 300)
        step("xv6 guest", xv6_guest)
        m = q.mark(); q.cmd_("echo hi from the guest; sleep 1; echo done", timeout=300); print(q.output_since(m))
        G.detach(q)
    if "linux" in steps:
        def linux_guest():
            # the command is in /lx (fpga/zedboard/lx): typed whole it is longer than the line
            # editor's width, and its redraws overran the 16-byte receive FIFO
            q.send("sh /lx\n")
            q.expect(r"vm (\d+) linux: ", 300)
            q.expect(r"Linux version 6\.", 3600)
            q.expect(r"Run /init as init process", 3600)
            q.expect(r"xv6h-linux: ready", 1800)
            q.expect(r"linux# ", 600)
        step("linux guest", linux_guest)
        print(L.linux_cmd(q, "uname -a; cat /proc/cpuinfo | head -8; sleep 1; date", 600))
        L.poweroff(q)
    if "save" in steps:
        # B.4, the board's half: a running Linux guest (no disk: initramfs only) is saved to a file
        # on the RAM disk, the RAM disk read out of DDR over JTAG, and the file taken out of it
        def lx_save():
            q.send("sh /lx\n")
            m = q.expect(r"vm (\d+) linux: ", 300)
            q.vmid = int(m.group(1))
            q.expect(r"linux# ", 3600)
            out = L.linux_cmd(q, "echo zed-marker-$$ > /keep; cat /keep; cat /proc/uptime", 300)
            q.marker = re.search(r"zed-marker-\d+", out).group(0)
            G.detach(q)
            q.send("vmm save %d /lx.vst\n" % q.vmid)
            q.expect(r"saved to /lx.vst \((\d+) KB\)", 1800)
            q.expect(r"\$ ", 120)
            q.send("sync; echo synced\n"); q.expect(r"synced", 600); q.expect(r"\$ ", 60)
            print("B4 saved on the board: marker %s" % q.marker); sys.stdout.flush()
        step("save on the board", lx_save)
        def dump():
            out = IMG + "/ramdisk-out.img"
            sh("docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb -v {F}:{F} kkrizka/vivado:2020.1 bash -lc "
               "\"source /opt/Xilinx/Vivado/2020.1/settings64.sh && xsdb {F}/zedboard/dump.tcl {o} 0x1a000000 {n}\"".format(
                   F=FPGA, o=out, n=90 << 20))
            sh("cp fpga/zedboard/dump.tcl %s/zedboard/ 2>/dev/null || true" % FPGA)
            # the journal replayed first: after sync the new file's metadata may be in it alone
            subprocess.run("e2fsck -fy %s > %s/ramdisk-fsck.log 2>&1" % (out, IMG), shell=True)
            sh("debugfs -R 'dump /lx.vst %s/lx.vst' %s && ls -la %s/lx.vst && echo %s > %s/lx.marker" % (IMG, out, IMG, q.marker, IMG))
        sh("cp fpga/zedboard/dump.tcl %s/zedboard/" % FPGA)
        step("read the RAM disk out", dump)
    if "restore" in steps:
        def lx_restore():
            marker = open(IMG + "/q.marker").read().strip()
            m = q.mark()
            q.send("vmm restore /q.vst -n q2 -d\n")
            q.expect(r"restored from /q.vst|cannot|refus|not ", 1800)
            q.expect(r"\$ ", 600)
            out = q.output_since(m)
            print("B4 restore on the board:\n" + "\n".join(l for l in out.splitlines() if "hyp:" in l or "vm " in l)[-2500:])
            if "restored from /q.vst" not in out:
                print("B4 REFUSED on the board"); sys.stdout.flush()
                return
            # where the arrived guest's time goes: exits by kind and cbo.zero done in software, 30 s apart
            for k in range(3):
                m = q.mark()
                q.send("vmm info q2 | grep -E 'exits|cpu:'; uptime\n"); q.expect(r"\$ ", 120)
                print("B4 info %d:\n%s" % (k, q.output_since(m).strip())); sys.stdout.flush()
                if k < 2:
                    q.send("sleep 30\n"); q.expect(r"\$ ", 120)
            # a profile from outside: vcpu0's pc, sampled (tools/pcprof.py maps it to the guest's symbols)
            pcs = []
            for k in range(int(os.environ.get("ZED_PCSAMPLES", "40"))):
                m = q.mark()
                q.send("vmm info q2 | grep -E 'vcpu0: pc|vsepc|vstimecmp|clock:|imsic0'\n"); q.expect(r"\$ ", 120)
                out = q.output_since(m)
                if k < 8:
                    print("B4 STATE %d:\n%s" % (k, out.strip())); sys.stdout.flush()
                pc = re.search(r"vcpu0: pc 0x([0-9a-f]+) last cause 0x([0-9a-f]+)", out)
                sepc = re.search(r"vsepc 0x([0-9a-f]+)", out)
                if pc:
                    pcs.append((pc.group(1), pc.group(2), sepc.group(1) if sepc else "-"))
            print("B4 PCS " + " ".join("%s/%s/%s" % p for p in pcs)); sys.stdout.flush()
            G.attach(q, "q2"); q.send("\n"); q.expect(r"linux# ", 900)
            out = L.linux_cmd(q, "cat /keep; cat /proc/uptime", 900)
            print("B4 guest on the board:\n" + out)
            out2 = L.linux_cmd(q, "dmesg | grep -E 'machrev|riscv-timer|vector' | tail -12", 900)
            print("B4 the guest's record:\n" + out2)
            assert marker in out, out
        step("restore on the board", lx_restore)
    if "revise" in steps:
        # B.3: facts revised under a running Linux on the hardware, each followed by the guest's
        # own oracle (its disk, written and read back); the adoption time is xv6H's own line
        def lx_start():
            q.send("sh /lxv\n")
            m = q.expect(r"vm (\d+) linux: ", 300)
            q.vmid = int(m.group(1))
            q.expect(r"xv6h-linux: ready", 3600)
            q.expect(r"linux# ", 600)
            out = L.linux_cmd(q, "mount /dev/vda /mnt && cat /mnt/marker; dmesg | grep xv6h-vic | head -1", 300)
            assert "marker-on-virtio-disk" in out, out
            G.detach(q)
        step("linux guest (vic, disk)", lx_start)
        results = []
        # F1 to an APLIC or IMSIC is refused on this host by xv6H's own rule (a host whose
        # controller is a PLIC cannot present the AIA's honestly): asked once, the refusal recorded
        q.send("vmm revise %d aplic\n" % q.vmid); q.expect(r"\$ ", 120)
        print("B3 aplic: " + q.output_since(q.mark() - 300)[-160:].replace("\n", " ")); sys.stdout.flush()
        for fact in ["intcmap alt", "intcmap default", "map alt", "map default",
                     "timebase 1000000", "timebase 400000", "sbi rfence", "sbi none",
                     "map alt", "map default", "timebase 100000", "timebase 400000"]:
            def one(fact=fact):
                mv = q.mark()
                q.send("vmm revise %d %s\n" % (q.vmid, fact))
                m = q.expect(r"revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", 300)
                us = int(m.group(3)); adopted = m.group(2).decode() if isinstance(m.group(2), bytes) else m.group(2)
                q.send("\n"); q.expect(r"\$ ", 120)
                G.attach(q, str(q.vmid)); q.send("\n"); q.expect(r"linux# ", 120)
                tag = fact.replace(" ", "-")
                out = L.linux_cmd(q, "echo at-%s > /mnt/b3 && sync && cat /mnt/b3; cat /proc/uptime" % tag, 600)
                assert ("at-%s" % tag) in out, out
                G.detach(q)
                results.append((fact, adopted, us))
                print("B3 %-18s adopted 0x%s after %d us; the disk answered" % (fact, adopted, us)); sys.stdout.flush()
            try:
                step("revise " + fact, one)
            except Exception:
                results.append((fact, "FAILED", 0))   # the next fact is still asked
        print("B3 SUMMARY", results)
finally:
    print("SUMMARY", ok, "total %.0fs" % (time.time() - t0))
    tail = q.buf[-3000:].decode(errors="replace")
    print("CONSOLE TAIL:\n" + tail)
