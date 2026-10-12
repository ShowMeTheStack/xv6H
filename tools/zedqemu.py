#!/usr/bin/env python3
# zedqemu: the board run on QEMU first: xv6H on a machine with the ZedBoard Rocket's ISA profile -- no Sstc, no
# `time' CSR, no hardware A/D updating, Sv39 only, no vector, no Zicbo*, PLIC, one hart, 256 MB
# with a RAM disk where the ARM will load it -- booted from the board's device tree (its QEMU
# variant: the UART's line and the timebase are QEMU's). Then an xv6 guest and a Linux guest.
#   tools/remote.sh script zedq tools/zedqemu.py
import os, re, shlex, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":     # run in place; remote.sh runs it from the tree's root
    HERE = os.path.dirname(HERE)
os.chdir(HERE)
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G, test_hyp_linux as L

def sh(c):
    print("+", c); sys.stdout.flush()
    subprocess.run(c, shell=True, check=True)

# the board's root: 90 MB (the RAM disk is 94), one xv6 guest disk, the Linux guest's kernel and disk
RESTORE = len(sys.argv) > 1 and sys.argv[1] == "restore"   # board -> QEMU on the stand-in: the QEMU-saved guest
IMG = os.path.expanduser("~/xv6h-fpga/zed-images")
sh("rm -f fs.img && make -s fs.img HOSTFS_MB=90 GUESTDISKS=1 HOSTFILES='%s' && mv fs.img zed-fs.img" %
   ("linux/Image" if RESTORE else "linux/Image linux/linux.img"))
if RESTORE:
    sh("debugfs -w -R 'write %s/q.vst /q.vst' zed-fs.img" % IMG)
sh("python3 fpga/zedboard/mkdts.py qemu > zed-qemu.dts && dtc -q -I dts -O dtb -o zed-qemu.dtb zed-qemu.dts")
CPU = ("rv64,h=true,v=false,sstc=false,zicntr=false,zihpm=true,sv48=false,sv57=false,svadu=false,"
       "svpbmt=false,svnapot=false,svinval=false,zicbom=false,zicboz=false,zicbop=false,zba=false,"
       "zbb=false,zbs=false,zihintpause=false,zawrs=false,zfa=false,zicond=false,zcb=false,zihintntl=false")
cmd = ("qemu-system-riscv64 -machine virt,aia=none -cpu %s -m 256M -smp 1 -bios none -nographic "
       "-kernel kernel/kernel -dtb zed-qemu.dtb -device loader,file=zed-fs.img,addr=0x8a000000,force-raw=on "
       "-net none" % CPU)
print("+", cmd); sys.stdout.flush()
q = object.__new__(T.QEMU)
q.cmd = cmd; q.log = open("zedq-console.log", "w")
q.proc = subprocess.Popen(shlex.split(cmd), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
q.buf = b""; q.consumed = 0; q.gone = False
ok = {}
def step(name, f):
    t0 = time.time()
    try:
        f(); ok[name] = "ok %.1fs" % (time.time() - t0)
    except Exception as e:
        ok[name] = "FAILED %.1fs: %s" % (time.time() - t0, str(e)[-1500:])
        raise
    finally:
        print("STEP", name, ok[name]); sys.stdout.flush()
try:
    step("host boot", lambda: (q.expect(r"hyp: (hypervisor extension present|no hypervisor extension)", 300),
                               q.expect(r"init: starting sh", 300), q.expect(r"\$ ", 60)))
    m = q.mark(); q.cmd_("cat /proc/cpuinfo", timeout=30); print(q.output_since(m))
    m = q.mark(); q.cmd_("dmesg | grep -i -E 'blk|plat|timer|sstc|time'", timeout=30); print(q.output_since(m))
    m = q.mark(); q.cmd_("sleep 2; date", timeout=60); print(q.output_since(m))
    if RESTORE:
        def restore():
            m = q.mark()
            q.send("vmm restore /q.vst -n q2 -d\n")
            q.expect(r"restored from /q.vst|cannot|refus", 900); q.expect(r"\$ ", 300)
            print("\n".join(l for l in q.output_since(m).splitlines() if "hyp:" in l or "vm " in l))
            for k in range(6):
                m = q.mark(); q.send("vmm info q2 | grep -E 'exits|vcpu0: pc|vstimecmp|clock:|imsic0|csr exits'\n"); q.expect(r"\$ ", 120)
                print("STATE %d:\n%s" % (k, q.output_since(m).strip())); sys.stdout.flush()
                q.send("sleep 5\n"); q.expect(r"\$ ", 60)
            G.attach(q, "q2"); q.send("\n"); q.expect(r"linux# ", 600)
            print(L.linux_cmd(q, "cat /keep; cat /proc/uptime; dmesg | grep -E 'machrev|riscv-timer' | tail -8", 300))
        step("restore", restore)
        raise SystemExit(0)
    step("xv6 guest", lambda: G.start_guest(q, "-n g1 -c 1 -m 32", 600))
    m = q.mark(); q.cmd_("echo hi from the guest; sleep 1; date", timeout=60); print(q.output_since(m))
    G.detach(q)
    L.LINUX_ARGS = L.LINUX_ARGS.replace("-m 256", "-m 80")    # the board's 160 MB, 48 kept by the host
    step("linux guest", lambda: L.start_linux(q, "-f none -c 1", name="linux", timeout=900))
    print(L.linux_cmd(q, "uname -a; cat /proc/cpuinfo | head -12; sleep 1; date", 120))
    L.poweroff(q)
finally:
    print("SUMMARY", ok)
    q.close()
