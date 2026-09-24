#!/usr/bin/env python3
"""
qrun: run the test matrix as a queue of one-test jobs.

test-hyp.py runs a suite's tests one after another, so a matrix takes as
long as its slowest *suite* -- and a machine with thirty-two cores sits
mostly idle while it does. This runs each test as its own job, several
at a time, so the matrix takes as long as its slowest *test*.

Three classes, because they cannot all share a machine the same way:

  exclusive  what measures time rather than behaviour: the scheduler's
             fairness and latency tests, the Linux SMP ratio. These run
             last, one at a time, with nothing else on the box, because
             a co-tenant changes the number they are reading.
  heavy      what pins its harts for minutes: anything running
             usertests, the big-memory tests, the process storms.
  light      everything else -- boots, device tests, the network --
             which spends most of its time waiting rather than running.

Each job gets its own disk image and its own console log, so jobs never
write over each other (which is how two harnesses in one tree produce
failures that are not there).

  tools/qrun.py                      the whole matrix
  tools/qrun.py -s guest             one suite
  tools/qrun.py -t guest-churn -r 20 one test, twenty times (flake hunting)
  tools/qrun.py --slots 16 --heavy 8
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
SUITES = ["native", "ns", "ct", "sched", "proc", "mm", "net", "guest", "linux",
          "snap"]

# what must not share the machine, and what is expensive to run.
#
# hostfiles-build is here for a different reason than the rest: it
# rebuilds fs.img in the tree, so any job that copies the image while
# it runs finds it missing. (That is what it did: "overlay: FAIL (0s):
# No such file or directory: fs.img".) A test that rewrites the build
# has to have the tree to itself.
EXCLUSIVE = [r"^sched-", r"^linux-disk-smp", r"^hostfiles-build$",
             # these ping the internet through qemu's NAT, and the
             # guest gives the reply three seconds. That deadline is
             # inside the guest, so no amount of scaling the harness's
             # own waits saves it: the test needs the machine quiet.
             r"^net-guest-dhcp", r"^net-linux-dhcp", r"^net-guest-static",
             r"^net-linux-static",
             # and these, measured rather than guessed: each one failed
             # in the queue and passed alone, at the same scaled waits.
             # guest-lifecycle proves a paused guest is silent, which is
             # a three-second window it cannot have stretched;
             # guest-pages counts free pages as they settle; and
             # linux-with-xv6 runs usertests in one guest while Linux
             # runs in another, which is already a full machine.
             r"^guest-lifecycle", r"^guest-pages", r"^linux-with-xv6"]
HEAVY = [r"^native-", r"usertests", r"^mm-", r"^proc-", r"^guest-fuzz",
         r"^guest-pages", r"^guest-parallel", r"^fsbench", r"^libc$",
         # two machines at once, or one of them worked hard
         r"^guest-ksm", r"^guest-churn", r"^guest-memlimit", r"^guest-smp",
         r"^guest-balloon", r"^containers$", r"^cgroups$",
         # a few thousand forks in containers, one process per call
         r"^sysfilter$", r"^ct-"]
# and the few that are heavy even among the heavy: two six-gigabyte
# guests running usertests at once, a half-terabyte machine, a hundred
# thousand processes. They cost more than three harts' worth of a
# machine -- memory bandwidth and disk, which no hart count measures --
# and running several beside an ordinary usertests run is what made
# mm-1g take fifteen minutes instead of fifty seconds.
HUGE = [r"^mm-16g$", r"^mm-512g$", r"^proc-100k$", r"^proc-8000$"]

def classify(name):
    for rx in EXCLUSIVE:
        if re.search(rx, name):
            return "exclusive"
    for rx in HUGE:
        if re.search(rx, name):
            return "huge"
    for rx in HEAVY:
        if re.search(rx, name):
            return "heavy"
    return "light"


def enumerate_tests(suites, extra=()):
    jobs = []
    for s in suites:
        out = subprocess.run([sys.executable, "./test-hyp.py", s, "--list"] + list(extra),
                             cwd=HERE, stdout=subprocess.PIPE, encoding="utf8")
        for line in out.stdout.splitlines():
            name = line.strip()
            if name and re.fullmatch(r"[a-z0-9][a-z0-9_.-]*", name):
                jobs.append((s, name))
    return jobs


class Job:
    def __init__(self, suite, name, slot, run, extra=()):
        self.suite, self.name, self.slot, self.run = suite, name, slot, run
        self.extra = list(extra)
        self.kind = classify(name)
        self.t0 = time.time()
        assert self.kind in ("light", "heavy", "huge", "exclusive")
        env = dict(os.environ,
                   TEST_FSIMG="q%d-fs.img" % slot,
                   TEST_LOG="q%d.log" % slot,
                   # a job sharing the machine gets longer to wait; one
                   # that has it to itself keeps the honest numbers.
                   TEST_TIMEOUT_SCALE="1" if self.kind == "exclusive" else "4")
        self.out = open(os.path.join(HERE, "qlogs", "%s.%d.out" % (name, run)), "w+")
        self.proc = subprocess.Popen(
            [sys.executable, "-u", "./test-hyp.py", suite, "-t", name, "--no-build"]
            + self.extra,
            cwd=HERE, env=env, stdout=self.out, stderr=subprocess.STDOUT)

    def done(self):
        return self.proc.poll() is not None

    def result(self):
        self.out.seek(0)
        txt = self.out.read()
        self.out.close()
        ok = re.search(r"^== %s: PASS" % re.escape(self.name), txt, re.M) is not None
        return ok, time.time() - self.t0, txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--suite", action="append", help="only these suites")
    ap.add_argument("-t", "--test", help="only tests whose name contains this")
    ap.add_argument("-r", "--repeat", type=int, default=1)
    ap.add_argument("--capacity", type=int, default=0,
                    help="harts to keep busy (default: the machine's cores less four)")
    ap.add_argument("--slots", type=int, default=0, help="most jobs at a time")
    ap.add_argument("--timeout", type=int, default=900,
                    help="seconds a light job may take (heavy jobs get four times "
                         "this, which is what mm-16g and proc-100k need)")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--axis", default=None, metavar="K=V,...",
                    help="the machine to run them on; passed to every job "
                         "(see test-hyp.py --axis)")
    ap.add_argument("--tier", default=None, choices=["quick", "full"],
                    help="which set of tests (see test-hyp.py --tier)")
    ap.add_argument("--json", default=None, metavar="FILE",
                    help="write the summary there as JSON")
    args = ap.parse_args()
    # what every job is told, over and above its own name
    extra = []
    if args.axis:
        extra += ["--axis", args.axis]
    if args.tier:
        extra += ["--tier", args.tier]

    # a test runs three harts, and a hart is a thread that spins when the
    # guest is busy. Admitting jobs by weight rather than by count is
    # what keeps a machine full without overloading it: at twice the
    # cores the tests still pass, but the ones that assert on a timeout
    # -- usertests has fifteen minutes, a guest has ninety seconds to
    # boot -- start to fail for want of a processor rather than for a
    # reason. Light jobs (a boot, a device, the network) spend their
    # time waiting and count as one.
    cores = os.cpu_count() or 8
    capacity = args.capacity or max(4, cores * 5 // 8)
    slots = args.slots or capacity
    os.makedirs(os.path.join(HERE, "qlogs"), exist_ok=True)
    if not args.no_build:
        print("building...", flush=True)
        subprocess.run(["make", "-s"], cwd=HERE, check=True)
        # the Linux artifacts, if they were built and fetched
        have_linux = (os.path.exists(os.path.join(HERE, "linux", "Image")) and
                      os.path.exists(os.path.join(HERE, "linux", "linux.img")))
        if have_linux:
            subprocess.run(["rm", "-f", "fs.img"], cwd=HERE)
            subprocess.run(["make", "-s", "fs.img", "HOSTFS_MB=192",
                            "HOSTFILES=linux/Image linux/linux.img"], cwd=HERE,
                           check=True, stdout=subprocess.DEVNULL)
        else:
            subprocess.run(["make", "-s", "fs.img"], cwd=HERE, check=True,
                           stdout=subprocess.DEVNULL)

    jobs = enumerate_tests(args.suite or SUITES, extra)
    if args.test:
        jobs = [j for j in jobs if args.test in j[1]]
    queue = [(s, n, r) for r in range(args.repeat) for (s, n) in jobs]
    parallel = [j for j in queue if classify(j[1]) != "exclusive"]
    serial = [j for j in queue if classify(j[1]) == "exclusive"]
    print("%d jobs on %d cores: %d in parallel (capacity %d harts), %d one at a time"
          % (len(queue), cores, len(parallel), capacity, len(serial)), flush=True)

    WEIGHT = {"light": 1, "heavy": 3, "huge": 8, "exclusive": capacity}
    results = []
    free_slots = list(range(slots))
    running = []
    t0 = time.time()

    def reap():
        for j in list(running):
            limit = args.timeout * {"light": 1, "heavy": 3, "huge": 5,
                                    "exclusive": 3}[j.kind]
            if j.done() or time.time() - j.t0 > limit:
                if not j.done():
                    j.proc.kill(); j.proc.wait()
                ok, dt, txt = j.result()
                results.append((j.name, j.run, ok, dt))
                running.remove(j)
                free_slots.append(j.slot)
                mark = "ok  " if ok else "FAIL"
                print("  %s %-34s %5.0fs  (%d/%d)"
                      % (mark, j.name, dt, len(results), len(queue)), flush=True)

    for phase, group, limit in (("parallel", parallel, slots),
                                ("serial", serial, 1)):
        pending = list(group)
        while pending or running:
            reap()
            while pending and len(running) < limit and free_slots:
                busy = sum(WEIGHT[j.kind] for j in running)
                huge_running = sum(1 for j in running if j.kind == "huge")
                pick = None
                for i, (s, n, r) in enumerate(pending):
                    # two of the huge ones together make both of them
                    # crawl, whatever the hart count allows
                    if classify(n) == "huge" and huge_running:
                        continue
                    if busy + WEIGHT[classify(n)] > capacity and running:
                        continue
                    pick = i
                    break
                if pick is None:
                    break
                s, n, r = pending.pop(pick)
                running.append(Job(s, n, free_slots.pop(0), r, extra))
            time.sleep(1)

    bad = [(n, r) for (n, r, ok, dt) in results if not ok]
    print("\n%d jobs in %.0fs; %d failed" % (len(results), time.time() - t0, len(bad)))
    for n, r in bad:
        print("  FAIL %s (run %d) -- qlogs/%s.%d.out" % (n, r, n, r))
    open(os.path.join(HERE, "qresults.txt"), "w").write(
        "".join("%s %d %s %.0f\n" % (n, r, "PASS" if ok else "FAIL", dt)
                for (n, r, ok, dt) in results))
    if args.json:
        import json
        with open(args.json, "w") as f:
            json.dump({"axis": args.axis or "", "tier": args.tier,
                       "suites": args.suite or SUITES,
                       "seconds": round(time.time() - t0, 1),
                       "jobs": len(results), "failed": len(bad),
                       "tests": [{"test": n, "run": r,
                                  "result": "pass" if ok else "fail",
                                  "seconds": round(dt, 1)}
                                 for (n, r, ok, dt) in results]}, f, indent=1)
            f.write("\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
