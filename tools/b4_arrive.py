#!/usr/bin/env python3
# B.4, the QEMU half: a machine saved on the ZedBoard (Rocket + H at 40 MHz: PLIC, 400 kHz, no
# vector unit, no Zicboz, no Sstc, one hart) is started again on a QEMU host of another shape
# (APLIC+IMSIC, 10 MHz, vector, Zicboz, Sstc, three harts). The file came out of the board's
# RAM disk over JTAG (tools/zedboard.py save). On arrival xv6H tells the guest what it has
# arrived on; the guest still holds what it was given on the board (a marker in its memory).
#   tools/remote.sh script b4 tools/b4_arrive.py [intc]
import os, re, shutil, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE)
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G, test_hyp_linux as L

IMG = os.path.expanduser("~/xv6h-fpga/zed-images")
intc = sys.argv[1] if len(sys.argv) > 1 else "imsic"
marker = open(IMG + "/lx.marker").read().strip()
print("from the board: %s (%d bytes), marker %s" % (IMG + "/lx.vst", os.path.getsize(IMG + "/lx.vst"), marker))
# the file into the host's root (the harness copies fs.img for each machine)
subprocess.run("debugfs -w -R 'write %s/lx.vst /lx.vst' fs.img" % IMG, shell=True, check=True)
q = G.boot_host(T.QEMU, intc, 3, open("b4-console.log", "w"))
ok = False
try:
    m = q.mark()
    q.send("vmm restore /lx.vst -n lx2 -d\n")
    q.expect(r"restored from /lx.vst", 600)
    q.expect(r"\$ ", 120)
    time.sleep(10)
    out = q.output_since(m)
    print("restore:\n" + "\n".join(l for l in out.splitlines() if "hyp:" in l or "vm " in l or "arriv" in l)[-3000:])
    G.attach(q, "lx2"); q.send("\n"); q.expect(r"linux# ", 300)
    out = L.linux_cmd(q, "cat /keep; cat /proc/uptime; uname -m", 300)
    print("guest:\n" + out)
    out2 = L.linux_cmd(q, "dmesg | grep -E 'machrev|riscv-timer|xv6h-vic' | tail -12", 300)
    print("guest's record of the arrival:\n" + out2)
    out3 = L.linux_cmd(q, "cat /proc/cpuinfo | grep -E 'isa|mmu' | head -3; echo after > /tmp/t; cat /tmp/t", 300)
    print(out3)
    ok = marker in out and "after" in out3
    L.poweroff(q)
finally:
    print("RESULT", "ok" if ok else "FAILED")
    q.close()
