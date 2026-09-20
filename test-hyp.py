#!/usr/bin/env python3
#
# xv6H test driver: boots xv6 under qemu in a given interrupt-controller
# mode, types at its console, and checks the output. no human needed.
#
#   ./test-hyp.py native            # host xv6 boots + usertests -q, all modes
#   ./test-hyp.py -i plic native    # one mode
#   ./test-hyp.py guest             # boot a guest, run usertests inside it
#   ./test-hyp.py linux             # Linux guests (needs linux/Image, see linux/build.sh)
#   ./test-hyp.py net               # networking: host, guests, TLS, egress policy
#   ./test-hyp.py mm                # memory: allocator, Sv48/Sv39, 16G and 512G hosts, on-demand guests
#   ./test-hyp.py proc              # processes: thousands at once, pid wraparound, everything reaped
#   ./test-hyp.py all               # everything
#
import argparse, os, re, select, shutil, subprocess, sys, time

sys.stdout.reconfigure(line_buffering=True)

HERE = os.path.dirname(os.path.abspath(__file__))
MODES = ["plic", "aplic", "imsic"]


class QEMU(object):
    def __init__(self, intc="plic", cpus=3, mem="1G", aia_guests=2, hyp=1, vec=1, vlen=128, log=None, extra_env=None):
        # every run starts from a pristine copy of the host file system
        # (TEST_FSIMG names the copy, so that two runs can go in parallel).
        fsimg = os.environ.get("TEST_FSIMG", "test-fs.img")
        env = dict(os.environ, INTC=intc, CPUS=str(cpus), MEM=mem, AIA_GUESTS=str(aia_guests),
                   HYP=str(hyp), VEC=str(vec), VLEN=str(vlen), FSIMG=fsimg)
        if extra_env:
            env.update(extra_env)
        shutil.copyfile(os.path.join(HERE, "fs.img"), os.path.join(HERE, fsimg))
        cmd = subprocess.run(["make", "-s", "--no-print-directory", "qemucmd"], cwd=HERE,
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             encoding="utf8", check=True).stdout.strip().split("\n")[-1]
        self.cmd = cmd
        self.log = log
        self.proc = subprocess.Popen(cmd.split(), cwd=HERE, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.buf = b""
        self.consumed = 0

    def _pump(self, timeout):
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r:
            return False
        d = os.read(self.proc.stdout.fileno(), 65536)
        if not d:
            return False
        self.buf += d
        if self.log:
            self.log.write(d.decode(errors="replace"))
            self.log.flush()
        return True

    def expect(self, pattern, timeout=60):
        """wait for a regex in output produced since the last expect()."""
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern, re.S)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf, self.consumed)
            if m:
                self.consumed = m.end()
                return m
            if time.time() > end:
                tail = self.buf[-3000:].decode(errors="replace")
                raise TimeoutError("timeout waiting for %r; tail:\n%s" % (pattern, tail))
            self._pump(min(1.0, max(0.05, end - time.time())))

    def send(self, s):
        self.proc.stdin.write(s.encode() if isinstance(s, str) else s)
        self.proc.stdin.flush()

    def cmd_(self, line, prompt=r"\$ ", timeout=60):
        self.send(line + "\n")
        return self.expect(prompt, timeout)

    def output_since(self, mark):
        return self.buf[mark:].decode(errors="replace")

    def mark(self):
        return len(self.buf)

    def close(self):
        try:
            self.proc.kill()
            self.proc.wait(5)
        except Exception:
            pass


def usertests_ok(q, prompt=r"\$ ", timeout=900):
    m = q.mark()
    q.send("usertests -q\n")
    q.expect(r"ALL TESTS PASSED|FAILED|panic", timeout)
    out = q.output_since(m)
    if "ALL TESTS PASSED" not in out:
        raise AssertionError("usertests failed:\n" + out[-4000:])
    q.expect(prompt, 30)
    return out


def t_native(intc, cpus, log, hyp=1, vec=1, vlen=128):
    q = QEMU(intc=intc, cpus=cpus, log=log, hyp=hyp, vec=vec, vlen=vlen)
    try:
        q.expect(r"xv6 kernel is booting", 30)
        m = q.expect(r"platform: .*?\n", 10)
        print("   ", m.group(0).decode().strip())
        if hyp:
            q.expect(r"hyp: hypervisor extension present", 10)
        else:
            q.expect(r"hyp: no hypervisor extension; guests disabled", 10)
        q.expect(r"init: starting sh", 30)
        q.expect(r"\$ ", 10)
        q.cmd_("echo native-ok")
        assert re.search(r"kthread: ok \(100 switches\)", q.buf.decode(errors="replace")), "kernel threads did not report"
        if not hyp:
            q.send("vmm start -d\n")
            q.expect(r"cannot create vm", 10)
            q.expect(r"\$ ", 5)
        out = usertests_ok(q)
        n = len(re.findall(r"test .*: OK", out))
        print("    usertests: %d tests OK" % n)
    finally:
        q.close()


# the C library's own tests, and the two that check the numerics.
def t_libc(log):
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        for prog, want in (("libctest", r"libctest: (\d+) checks, 0 failures"),
                           ("mathtest", r"math: (\d+) tests, 0 failures"),
                           ("fltest", r"float: (\d+) tests, 0 failures"),
                           ("regextest", r"regex: (\d+) checks, 0 failures"),
                           ("wctest", r"wctest: (\d+) checks, 0 failures"),
                           ("editest", r"editline: (\d+) checks, 0 failures"),
                           ("sh /shtest", r"shtest: (\d+) checks, 0 failures")):
            m = q.mark()
            q.send(prog + "\n")
            q.expect(r"\d+ (?:checks|tests), \d+ failures?\r?\n|panic|FAIL", 600)
            out = q.output_since(m)
            assert re.search(want, out), out[-3000:]
            print("    %s: %s" % (prog, re.search(want, out).group(0)))
            q.expect(r"\$ ", 30)
    finally:
        q.close()


def t_fsbench(log):
    """the file system benchmark runs to the end and reports every
    phase. what the numbers mean is in bench/RESULTS.md; what this test
    is for is that none of the phases has stopped working, which is how
    a file system regression shows up before anyone reads a number."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        m = q.mark()
        q.send("fsbench -s 4 -n 40\n")
        q.expect(r"fsbench: done|panic", 600)
        out = q.output_since(m)
        assert "fsbench: done" in out, out[-2000:]
        phases = {m2.group(1): (float(m2.group(2)), float(m2.group(3)))
                  for m2 in re.finditer(r"fsbench: (\w+)=[\d.]+ secs=([\d.]+) rate=([\d.]+)",
                                        out)}
        for want in ("seqwrite", "seqread", "randread", "randwrite", "fsyncsmall", "metadata",
                     "bigdir", "mixed"):
            assert want in phases, (want, out[-2000:])
            secs, rate = phases[want]
            assert rate > 0, (want, secs, rate)
        print("    " + "  ".join("%s %.0f" % (k, v[1]) for k, v in sorted(phases.items())))
        q.expect(r"\$ ", 30)
    finally:
        q.close()


def t_tty(log):
    """the console as a terminal: Ctrl-C reaches the foreground job and
    not the shell, Ctrl-Z stops it, and the shell survives both."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 40)
        q.expect(r"\$ ", 10)
        # Ctrl-C kills the foreground job, the shell comes back
        q.send("loopsig\n")
        q.expect(r"ready", 20)
        q.send("\x03")
        q.expect(r"\$ ", 20)
        q.cmd_("echo alive-after-intr")
        assert "alive-after-intr" in q.output_since(q.mark() - 200)
        # Ctrl-Z stops it; the shell reports the stop and carries on
        q.send("loopsig\n")
        q.expect(r"ready", 20)
        q.send("\x1a")
        q.expect(r"[Ss]topped", 20)   # the shell says "Stopped", the way bash does
        q.expect(r"\$ ", 20)
        m = q.mark()
        q.cmd_("echo alive-after-stop")
        assert "alive-after-stop" in q.output_since(m), q.output_since(m)
        # the shell still runs commands after all of that
        m = q.mark()
        q.cmd_("echo done")
        assert "done" in q.output_since(m), q.output_since(m)
    finally:
        q.close()


def t_clock(log, have_linux):
    """the host, an xv6 guest and a Linux guest all read the laptop's clock
    through the (virtual) goldfish RTC."""
    import test_hyp_guest as g
    q = g.boot_host(QEMU, "aplic", 3, log)

    def check(out, what, slack=300):
        m = re.search(r"(?m)^(\d{9,})\s*$", out)
        assert m, (what, out)
        secs = int(m.group(1))
        assert abs(secs - time.time()) < slack, (what, secs, time.time())
        return secs

    try:
        m = q.mark()
        q.cmd_("date -s")
        check(q.output_since(m), "host")
        m = q.mark()
        q.cmd_("date")
        assert re.search(r"[A-Z][a-z]{2} [A-Z][a-z]{2} [ \d]\d \d\d:\d\d:\d\d UTC 20\d\d", q.output_since(m)), q.output_since(m)
        g.start_guest(q, "-n g1 -c 2", 90)
        m = q.mark()
        q.cmd_("date -s")
        check(q.output_since(m), "xv6 guest")
        m = q.mark()
        q.cmd_("usertests clocktest", timeout=60)
        assert "ALL TESTS PASSED" in q.output_since(m), q.output_since(m)
        g.detach(q)
        if have_linux:
            import test_hyp_linux as L
            L.start_linux(q, "-f none -c 1", name="linux", timeout=150)
            out = L.linux_cmd(q, "date +%s; date; cat /sys/class/rtc/rtc0/name")
            check(out, "linux guest")
            assert "goldfish" in out, out
            L.poweroff(q)
        q.send("vmm kill --all\n")
        q.expect(r"\$ ", 30)
    finally:
        q.close()


def t_hostfiles(log):
    """make fs.img HOSTFILES=... puts extra files into the host image."""
    extra = os.path.join(HERE, "guest", "extra.txt")
    with open(extra, "w") as f:
        f.write("hello from HOSTFILES\n")
    try:
        subprocess.run(["make", "-s", "fs.img", "HOSTFILES=" + extra], cwd=HERE, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        q = QEMU(log=log)
        try:
            q.expect(r"\$ ", 60)
            q.send("cat extra.txt\n")
            q.expect(r"hello from HOSTFILES", 10)
        finally:
            q.close()
    finally:
        os.unlink(extra)
        subprocess.run(["rm", "-f", "fs.img"], cwd=HERE)
        import test_hyp_linux
        cmd = ["make", "-s", "fs.img"]
        if test_hyp_linux.available(HERE):
            cmd += ["HOSTFS_MB=128", "HOSTFILES=linux/Image linux/linux.img"]
        subprocess.run(cmd, cwd=HERE, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("suite", nargs="?", default="all")
    ap.add_argument("-i", "--intc", default=None, help="plic|aplic|imsic (default: all)")
    ap.add_argument("-c", "--cpus", type=int, default=3)
    ap.add_argument("-v", action="store_true", help="tee qemu output to stdout")
    ap.add_argument("-t", "--test", default=None, help="only tests whose name contains this")
    args = ap.parse_args()
    modes = [args.intc] if args.intc else MODES
    log = sys.stdout if args.v else open(os.path.join(HERE, "test-hyp.log"), "w")

    subprocess.run(["make", "-s"], cwd=HERE, check=True)
    import test_hyp_linux
    have_linux = test_hyp_linux.available(HERE)
    if have_linux:
        # the host image carries the Linux kernel and its disk as well
        # (make cannot tell that HOSTFILES changed: start from scratch).
        subprocess.run(["rm", "-f", "fs.img"], cwd=HERE)
        subprocess.run(["make", "-s", "fs.img", "HOSTFS_MB=128",
                        "HOSTFILES=linux/Image linux/linux.img"], cwd=HERE, check=True,
                       stdout=subprocess.DEVNULL)
    elif args.suite in ("linux", "all"):
        print("linux/Image not found: run linux/build.sh on a Linux box and linux/fetch.sh; skipping Linux tests")

    failures = 0
    tests = []
    if args.suite in ("native", "all"):
        for m in modes:
            tests.append(("native-%s" % m, lambda m=m: t_native(m, args.cpus, log)))
        if args.intc in (None, "plic"):
            tests.append(("native-no-hypervisor", lambda: t_native("plic", args.cpus, log, hyp=0)))
            tests.append(("native-no-vector", lambda: t_native("plic", args.cpus, log, vec=0)))
            tests.append(("native-vlen-512", lambda: t_native("plic", args.cpus, log, vlen=512)))
            tests.append(("hostfiles-build", lambda: t_hostfiles(log)))
            tests.append(("clock", lambda: t_clock(log, have_linux)))
            tests.append(("tty", lambda: t_tty(log)))
            tests.append(("libc", lambda: t_libc(log)))
            tests.append(("fsbench", lambda: t_fsbench(log)))
    if args.suite in ("guest", "all"):
        import test_hyp_guest  # milestone 2+
        tests += test_hyp_guest.tests(modes, args.cpus, log, QEMU, usertests_ok)
    if args.suite in ("linux", "all") and have_linux:
        tests += test_hyp_linux.tests(modes, args.cpus, log, QEMU)
    if args.suite in ("net", "all"):
        import test_hyp_net
        tests += test_hyp_net.tests(modes, args.cpus, log, QEMU, have_linux)
    if args.suite in ("mm", "all"):
        import test_hyp_mm
        tests += test_hyp_mm.tests(modes, args.cpus, log, QEMU, have_linux)
    if args.suite in ("proc", "all"):
        import test_hyp_proc
        tests += test_hyp_proc.tests(modes, args.cpus, log, QEMU)

    if args.test:
        tests = [t for t in tests if args.test in t[0]]
    for name, fn in tests:
        print("== %s" % name)
        t0 = time.time()
        try:
            fn()
            print("== %s: PASS (%.0fs)" % (name, time.time() - t0))
        except Exception as e:
            failures += 1
            import traceback
            msg = str(e)[-3000:]
            if not msg.strip():
                msg = traceback.format_exc().strip().splitlines()[-3:]
                msg = " | ".join(l.strip() for l in msg)
            print("== %s: FAIL (%.0fs): %s" % (name, time.time() - t0, msg))
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
