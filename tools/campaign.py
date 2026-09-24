#!/usr/bin/env python3
"""
the campaign: the same tests, over and over, on machines of different
shapes, with the benchmarks at the end (§11.3).

    tools/remote.sh script campaign tools/campaign.py

§11.1 lists the axes -- interrupt controller, harts, RAM, paging,
hypervisor, vector, network, disks, journal mode, guest pages, tick,
linking. Their product is thousands of runs and most of it says nothing:
the interrupt controller and the hart count interact, the paging mode and
the RAM size interact, and the rest are independent of one another. What
this runs instead is one combination per thing that can differ, against a
baseline that fixes everything else -- which is the restriction §11.3
asks for, and is why the list below is hand-written and commented rather
than generated.

Each row is a machine; each machine gets the quick tier (thirty-six
tests, one of each kind of thing), and the baseline machine also gets the
whole matrix. Then the benchmarks: the file system against Linux on the
same image (bench/run.py), the scheduler, and the hypervisor's own exit
costs from inside a guest (guest/vmbench).

What comes out is one JSON file per machine in bench/campaign/, and a
markdown summary on standard output which is what bench/RESULTS.md is
written from.
"""
import argparse, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
if not os.path.exists(os.path.join(HERE, "test-hyp.py")):
    HERE = os.getcwd()
OUT = os.path.join(HERE, "bench", "campaign")

# The machines. `axis' is what test-hyp.py --axis takes; `why' is what
# the row is for, and goes into the report.
MACHINES = [
    ("baseline",   "",                    "three harts, 1 GB, Sv48, PLIC, vector on"),
    ("aplic",      "intc=aplic",          "the AIA's wired controller"),
    ("imsic",      "intc=imsic",          "message-signalled interrupts in hardware"),
    ("uniprocessor", "cpus=1",            "one hart: no parallelism to hide behind"),
    ("eight-harts", "cpus=8",             "more harts than the tests expect"),
    ("sv39",       "sv=39",               "three-level paging"),
    ("ram-16g",    "mem=16G",             "sixteen gigabytes of guest-capable RAM"),
    ("no-vector",  "vec=0",               "no V extension: the fallbacks are used"),
    ("vlen-512",   "vlen=512",            "wider vectors"),
    ("no-hyp",     "hyp=0",               "no hypervisor extension: the host alone"),
]

# what the quick tier cannot ask for on a machine that has no hypervisor
NO_HYP_SUITES = ["native", "ns", "ct", "sched", "proc", "mm", "net"]


def run(cmd, **kw):
    print("+ %s" % " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=HERE, **kw)


def campaign(args):
    os.makedirs(OUT, exist_ok=True)
    rows = []
    # built once: every job below is told --no-build, because twelve
    # makes in one tree race each other (tools/qrun.py says so too).
    if not args.no_build:
        run(["make", "-s"], check=True)
        have_linux = all(os.path.exists(os.path.join(HERE, "linux", f))
                         for f in ("Image", "linux.img"))
        run(["rm", "-f", "fs.img"], check=True)
        if have_linux:
            run(["make", "-s", "fs.img", "HOSTFS_MB=192",
                 "HOSTFILES=linux/Image linux/linux.img"], check=True)
        else:
            run(["make", "-s", "fs.img"], check=True)

    for name, axis, why in MACHINES:
        if args.only and not re.search(args.only, name):
            continue
        js = os.path.join(OUT, "%s.json" % name)
        cmd = [sys.executable, "tools/qrun.py", "--no-build", "--tier", "quick",
               "--json", js]
        if axis:
            cmd += ["--axis", axis]
        if "hyp=0" in axis:
            for s in NO_HYP_SUITES:
                cmd += ["-s", s]
        t0 = time.time()
        run(cmd)
        try:
            with open(js) as f:
                r = json.load(f)
        except OSError:
            rows.append((name, why, 0, 0, time.time() - t0))
            continue
        rows.append((name, why, r["jobs"], r["failed"], r["seconds"]))

    # the baseline gets the whole matrix as well, which is the "full"
    # tier: everything, every interrupt controller, the long tests.
    if not args.quick_only and (not args.only or re.search(args.only, "full")):
        js = os.path.join(OUT, "full.json")
        run([sys.executable, "tools/qrun.py", "--no-build", "--json", js])
        try:
            with open(js) as f:
                r = json.load(f)
            rows.append(("full matrix", "every test, every controller",
                         r["jobs"], r["failed"], r["seconds"]))
        except OSError:
            pass

    print()
    print("| machine | what it is | tests | failed | time |")
    print("|---|---|---|---|---|")
    for name, why, jobs, failed, secs in rows:
        print("| `%s` | %s | %d | %d | %.0f min |"
              % (name, why, jobs, failed, secs / 60.0))
    bad = sum(f for (_, _, _, f, _) in rows)
    print()
    print("%d machines, %d tests, %d failed"
          % (len(rows), sum(j for (_, _, j, _, _) in rows), bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=None, metavar="PATTERN",
                    help="only the machines whose name matches")
    ap.add_argument("--quick-only", action="store_true",
                    help="skip the full matrix on the baseline")
    ap.add_argument("--no-build", action="store_true")
    return campaign(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
