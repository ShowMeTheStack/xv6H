#!/usr/bin/env python3
#
# xv6H test driver: boots xv6 under qemu in a given interrupt-controller
# mode, types at its console, and checks the output. no human needed.
#
#   ./test-hyp.py native            # host xv6 boots + usertests -q, all modes
#   ./test-hyp.py -i plic native    # one mode
#   ./test-hyp.py guest             # boot a guest, run usertests inside it
#   ./test-hyp.py all               # everything
#
import argparse, os, re, select, shutil, subprocess, sys, time

sys.stdout.reconfigure(line_buffering=True)

HERE = os.path.dirname(os.path.abspath(__file__))
MODES = ["plic", "aplic", "imsic"]


class QEMU(object):
    def __init__(self, intc="plic", cpus=3, mem="1G", aia_guests=2, hyp=1, log=None, extra_env=None):
        env = dict(os.environ, INTC=intc, CPUS=str(cpus), MEM=mem, AIA_GUESTS=str(aia_guests),
                   HYP=str(hyp), FSIMG="test-fs.img")
        if extra_env:
            env.update(extra_env)
        # every run starts from a pristine copy of the host file system.
        shutil.copyfile(os.path.join(HERE, "fs.img"), os.path.join(HERE, "test-fs.img"))
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


def t_native(intc, cpus, log, hyp=1):
    q = QEMU(intc=intc, cpus=cpus, log=log, hyp=hyp)
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
        if not hyp:
            q.send("vmm start -d\n")
            q.expect(r"cannot create vm", 10)
            q.expect(r"\$ ", 5)
        out = usertests_ok(q)
        n = len(re.findall(r"test .*: OK", out))
        print("    usertests: %d tests OK" % n)
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
        subprocess.run(["make", "-s", "fs.img"], cwd=HERE, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("suite", nargs="?", default="all")
    ap.add_argument("-i", "--intc", default=None, help="plic|aplic|imsic (default: all)")
    ap.add_argument("-c", "--cpus", type=int, default=3)
    ap.add_argument("-v", action="store_true", help="tee qemu output to stdout")
    args = ap.parse_args()
    modes = [args.intc] if args.intc else MODES
    log = sys.stdout if args.v else open(os.path.join(HERE, "test-hyp.log"), "w")

    subprocess.run(["make", "-s"], cwd=HERE, check=True)

    failures = 0
    tests = []
    if args.suite in ("native", "all"):
        for m in modes:
            tests.append(("native-%s" % m, lambda m=m: t_native(m, args.cpus, log)))
        if args.intc in (None, "plic"):
            tests.append(("native-no-hypervisor", lambda: t_native("plic", args.cpus, log, hyp=0)))
            tests.append(("hostfiles-build", lambda: t_hostfiles(log)))
    if args.suite in ("guest", "all"):
        import test_hyp_guest  # milestone 2+
        tests += test_hyp_guest.tests(modes, args.cpus, log, QEMU, usertests_ok)

    for name, fn in tests:
        print("== %s" % name)
        t0 = time.time()
        try:
            fn()
            print("== %s: PASS (%.0fs)" % (name, time.time() - t0))
        except Exception as e:
            failures += 1
            print("== %s: FAIL (%.0fs): %s" % (name, time.time() - t0, str(e)[-3000:]))
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
