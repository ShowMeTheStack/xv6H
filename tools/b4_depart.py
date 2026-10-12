#!/usr/bin/env python3
# QEMU -> board, the QEMU half: a running Linux guest (no disk, 64 MB, one vcpu) is saved on
# a QEMU host (APLIC+IMSIC, 10 MHz, Sstc, Zicboz; VEC=0|1 for the vector unit) and the file left
# for the board (tools/zedboard.py restore puts it in the board's root and restores it there).
#   tools/remote.sh script b4d tools/b4_depart.py [vec]
import os, re, subprocess, sys, time
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
vec = int(sys.argv[1]) if len(sys.argv) > 1 else 0
sv39 = len(sys.argv) > 2 and sys.argv[2] == "sv39"     # Linux's own choice of paging mode, made Sv39 (no4lvl)
q = G.boot_host(T.QEMU, "imsic", 3, open("b4d-console.log", "w"), vec=vec)
ok = False
try:
    L.LINUX_ARGS = L.LINUX_ARGS.replace("-m 256", "-m 64") + (" no4lvl" if sv39 else "")
    L.start_linux(q, "-f none -c 1", name="linux", timeout=300)
    out = L.linux_cmd(q, "echo qemu-marker-$$ > /keep; cat /keep; dmesg | grep machrev | tail -1", 120)
    marker = re.search(r"qemu-marker-\d+", out).group(0)
    G.detach(q)
    for k in range(3):
        m = q.mark(); q.send("vmm info linux | grep -E 'imsic0|vsie'\n"); q.expect(r"\$ ", 60)
        print("before the save %d:\n%s" % (k, q.output_since(m).strip())); sys.stdout.flush()
        time.sleep(1)
    m = q.mark()
    q.send("vmm save linux /q.vst\n")
    q.expect(r"saved to /q.vst \((\d+) KB\)", 600)
    q.expect(r"\$ ", 60)
    q.send("sync; echo synced\n"); q.expect(r"synced", 120); q.expect(r"\$ ", 60)
    print(q.output_since(m)[-300:])
    ok = True
finally:
    q.close()
img = os.environ.get("TEST_FSIMG", "test-fs.img")
subprocess.run("e2fsck -fy %s > /dev/null 2>&1; debugfs -R 'dump /q.vst %s/q.vst' %s" % (img, IMG, img), shell=True, check=True)
open(IMG + "/q.marker", "w").write(marker + "\n")
print("left for the board: %s/q.vst (%d bytes), marker %s, vector %d" % (IMG, os.path.getsize(IMG + "/q.vst"), marker, vec))
print("RESULT", "ok" if ok else "FAILED")
