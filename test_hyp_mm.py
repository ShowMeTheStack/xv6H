#
# memory tests for test-hyp.py: the buddy allocator over the lazily
# initialized page database, Sv48 paging (and the Sv39 fallback), hosts of
# 16 GiB and 512 GiB, guests whose RAM is backed on demand.
#
import re, time
import test_hyp_guest as g
import test_hyp_linux as L


def run(q, line, timeout=60):
    m = q.mark()
    q.cmd_(line, timeout=timeout)
    return q.output_since(m)


def boot(QEMU, log, mem, sv=48, cpus=3, intc="plic"):
    extra = {"SV": str(sv)}
    t0 = time.time()
    q = QEMU(intc=intc, cpus=cpus, mem=mem, log=log, extra_env=extra)
    q.expect(r"init: starting sh", 180)
    q.expect(r"\$ ", 10)
    txt = open(log.name).read() if hasattr(log, "name") else ""
    return q, time.time() - t0


def banner(q):
    """the kalloc and platform lines of the most recent boot."""
    txt = q.buf.decode(errors="replace")
    plat = re.findall(r"platform: .*?\n", txt)[-1]
    kal = re.findall(r"kalloc: (\d+) MB in (\d+) blocks, page database (\d+) MB", txt)[-1]
    return plat, int(kal[0]), int(kal[1]), int(kal[2])


def free_mb(q):
    out = run(q, "vmm stats", 20)
    m = re.search(r"free guest memory: (\d+) MB", out)
    assert m, out
    return int(m.group(1))


def t_mm_1g(QEMU, intc, cpus, log):
    """the default host: Sv48, the allocator reports the RAM, guests take
    memory only as they touch it and give it all back."""
    q, dt = boot(QEMU, log, "1G", 48, cpus, intc)
    try:
        plat, mb, nblocks, dbmb = banner(q)
        assert "sv48 paging" in plat, plat
        assert 1000 <= mb <= 1024 and nblocks < 64 and dbmb <= 16, (mb, nblocks, dbmb)
        f0 = free_mb(q)
        g.start_guest(q, "-n a -m 512 -c 2", 90)
        g.detach(q)
        out = run(q, "vmm info a")
        m = re.search(r"512 MB \((\d+) MB resident\)", out)
        assert m and int(m.group(1)) < 200, out  # an idle xv6 guest touches little of its RAM
        f1 = free_mb(q)
        assert f0 - f1 < 260, (f0, f1)
        g.kill_vm(q, "a")
        f2 = free_mb(q)
        assert abs(f2 - f0) <= 2, (f0, f2)
        assert "ALL TESTS PASSED" in run(q, "usertests -q", 900)
    finally:
        q.close()


def t_mm_sv39(QEMU, intc, cpus, log):
    """the Sv39 fallback: three-level tables, everything still works."""
    q, dt = boot(QEMU, log, "1G", 39, cpus, intc)
    try:
        plat, mb, nblocks, dbmb = banner(q)
        assert "sv39 paging" in plat, plat
        out = run(q, "usertests -q", 900)
        assert "ALL TESTS PASSED" in out, out[-2000:]
        g.start_guest(q, "-n a -m 256", 90)
        assert "ALL TESTS PASSED" in run(q, "usertests -q", 900)
        g.detach(q)
        g.kill_vm(q, "a")
    finally:
        q.close()


def t_mm_16g(QEMU, intc, cpus, log):
    """16 GiB: two 6 GiB guests run usertests side by side; the host is
    not exhausted because guest RAM is backed on demand."""
    q, dt = boot(QEMU, log, "16G", 48, cpus, intc)
    try:
        plat, mb, nblocks, dbmb = banner(q)
        assert 16000 <= mb <= 16384 and dbmb <= 200, (mb, dbmb)
        g.start_guest(q, "-n a -m 6144 -c 2 -f /disk0.img", 90)
        m = q.mark()
        q.send("usertests -q\n")
        g.detach(q)
        g.start_guest(q, "-n b -m 6144 -c 2 -f /disk1.img", 90)
        assert "ALL TESTS PASSED" in run(q, "usertests -q", 900)
        g.detach(q)
        g.attach(q, "a")
        q.expect(r"ALL TESTS PASSED", 900)
        q.expect(r"\$ ", 10)
        g.detach(q)
        out = run(q, "vmm info a")
        assert re.search(r"6144 MB \(\d+ MB resident\)", out), out
        run(q, "vmm kill --all", 60)
    finally:
        q.close()


def t_mm_512g(QEMU, intc, cpus, log, have_linux):
    """512 GiB: boots in under a few seconds because the page database
    is initialized lazily; a 256 GiB Linux guest boots; memory that is
    never touched is never committed by qemu."""
    q, dt = boot(QEMU, log, "512G", 48, cpus, intc)
    try:
        assert dt < 60, dt
        plat, mb, nblocks, dbmb = banner(q)
        assert 519000 <= mb <= 524288 and nblocks < 64 and 5000 <= dbmb <= 5200, (mb, nblocks, dbmb)
        assert free_mb(q) > 519000
        g.start_guest(q, "-n big -m 8192 -c 2", 120)
        assert "ALL TESTS PASSED" in run(q, "usertests -q", 900)
        g.detach(q)
        g.kill_vm(q, "big")
        if have_linux:
            t0 = time.time()
            L.start_linux(q, "-f none -c 2 -H -m 262144", name="l256", timeout=900)
            out = L.linux_cmd(q, "free -m | head -2", 60)
            m = re.search(r"Mem:\s+(\d+)", out)
            assert m and int(m.group(1)) > 250000, out
            L.poweroff(q)
            print("    256 GiB Linux guest: %.0fs" % (time.time() - t0))
        assert free_mb(q) > 519000
    finally:
        q.close()


def tests(modes, cpus, log, QEMU, have_linux):
    m = modes[0]
    return [
        ("mm-1g", lambda: t_mm_1g(QEMU, m, cpus, log)),
        ("mm-sv39", lambda: t_mm_sv39(QEMU, m, cpus, log)),
        ("mm-16g", lambda: t_mm_16g(QEMU, m, cpus, log)),
        ("mm-512g", lambda: t_mm_512g(QEMU, m, cpus, log, have_linux)),
    ]
