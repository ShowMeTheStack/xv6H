#!/usr/bin/env python3
#
# the benchmark campaign for the file system: fsbench on xv6H's own root,
# and fsbench in a Linux guest on a disk of the same size, made by the
# same mkext4, under the same QEMU. both sides therefore see 4096-byte
# blocks, a 1024-block journal and the same geometry; what differs is
# the kernel underneath and, for Linux, a layer of hypervisor.
#
# run it on the build box, where the riscv64 Linux compiler lives:
#
#   tools/remote.sh script bench bench/run.py
#
# it prints one line per phase per system and then a table, which is
# what bench/RESULTS.md is written from.
#
import os, re, subprocess, sys, time

# the tree is wherever test-hyp.py is: this script is run both from the
# repository (bench/run.py) and from a copy of the tree on the build
# box, where remote.sh puts it at the top as run_script.py.
HERE = os.path.dirname(os.path.abspath(__file__))
TOP = HERE if os.path.exists(os.path.join(HERE, "test-hyp.py")) else os.path.dirname(HERE)
if not os.path.exists(os.path.join(TOP, "test-hyp.py")):
    TOP = os.getcwd()
BENCH = os.path.join(TOP, "bench")
sys.path.insert(0, TOP)

from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", os.path.join(TOP, "test-hyp.py")).load_module()
L = SourceFileLoader("L", os.path.join(TOP, "test_hyp_linux.py")).load_module()

SIZE_MB = os.environ.get("BENCH_MB", "8")
NFILES = os.environ.get("BENCH_FILES", "100")
ARGS = "-s %s -n %s" % (SIZE_MB, NFILES)


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=TOP, check=True, **kw)


def parse(out):
    """the machine-readable lines fsbench prints: one per phase."""
    r = {}
    for m in re.finditer(r"fsbench: (\w+)=([\d.]+) secs=([\d.]+) rate=([\d.]+) unit=(\S+)", out):
        r[m.group(1)] = (float(m.group(4)), m.group(5))
    return r


def run_xv6():
    # the same image the Linux guest gets, attached as a second disk, so
    # that both measure a file system with the same geometry, the same
    # journal and the same free space rather than whatever their root
    # happens to be.
    sh("cp bench/bench.img bench/bench-xv6.img")
    q = th.QEMU(intc="plic", cpus=3, log=None,
                extra_env={"EXTRADISK": os.path.join(TOP, "bench/bench-xv6.img")})
    try:
        q.expect(r"init: starting sh", 180)
        q.expect(r"\$ ", 10)
        q.send("mkdir /m\n")
        q.expect(r"\$ ", 10)
        q.send("mount -t ext4 /dev/vdb /m\n")
        q.expect(r"mount: .* on /m type ext4", 20)
        q.expect(r"\$ ", 10)
        m = q.mark()
        q.send("fsbench %s -d /m/bench\n" % ARGS)
        end = time.time() + 900
        while time.time() < end:
            q._pump(1.0)
            if "fsbench: done" in q.output_since(m):
                break
        out = q.output_since(m)
        print(out.replace("\r", ""))
        return parse(out)
    finally:
        q.close()


def run_linux():
    q = th.QEMU(intc="plic", cpus=3, log=None)
    try:
        q.expect(r"init: starting sh", 180)
        q.expect(r"\$ ", 10)
        L.start_linux(q, "-f /bench.img -c 1", name="bench", timeout=180)
        out = L.linux_cmd(q, "mount -t ext4 /dev/vda /mnt && ls /mnt", 60)
        assert "fsbench" in out, out
        out = L.linux_cmd(q, "/mnt/fsbench %s -d /mnt/bench" % ARGS, 900)
        print(out.replace("\r", ""))
        r = parse(out)
        L.linux_cmd(q, "umount /mnt", 60)
        L.poweroff(q)
        return r
    finally:
        q.close()


def main():
    if not os.path.exists(os.path.join(BENCH, "bench.img")):
        sh("sh bench/build-linux.sh")
    sh("rm -f fs.img")
    sh("make -s fs.img HOSTFS_MB=192 "
       "HOSTFILES='linux/Image linux/linux.img bench/bench.img'",
       stdout=subprocess.DEVNULL)

    print("== xv6H, its own root")
    a = run_xv6()
    print("== Linux 6.12 as a guest, ext4 on /dev/vda")
    b = run_linux()

    print("\n| phase | xv6H | Linux guest | ratio | unit |")
    print("|---|---|---|---|---|")
    for k in ("seqwrite", "seqread", "randread", "randwrite", "fsyncsmall", "metadata", "bigdir",
              "mixed"):
        if k not in a or k not in b:
            continue
        x, unit = a[k]
        y, _ = b[k]
        ratio = (y / x) if x else 0
        print("| %s | %.2f | %.2f | %.2fx | %s |" % (k, x, y, ratio, unit))


main()
