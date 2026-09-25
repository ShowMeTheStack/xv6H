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

# What the quick tier can ask for on a machine that has no hypervisor
# extension: everything that does not start a guest. The guest, linux and
# snap suites are guests by definition; the mm suite is *about* guest
# memory (mm-1g starts one and waits for its vcpu, which on this machine
# never comes); and what is left -- the kernel and userland, the
# namespaces, the containers, the scheduler, the processes and the host's
# own network -- is the part of this system that a machine without the H
# extension still has.
NO_HYP_SUITES = ["native", "ns", "ct", "sched", "proc", "net"]


def run(cmd, **kw):
    print("+ %s" % " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=HERE, **kw)


# the pattern is bracketed so that it cannot match a command line that
# merely mentions the emulator -- a shell, an ssh, a wrapper script. pgrep
# and pkill leave themselves out but not their neighbours, and killing the
# shell this runs in has happened.
EMU = "qemu-system-riscv6[4]"


def quiet():
    return subprocess.run(["pgrep", "-f", EMU],
                          stdout=subprocess.DEVNULL).returncode != 0


def wait_quiet(limit=90):
    """see the machines of the last row gone before the next one starts.

    Every row uses the same disk images (q0-fs.img and its friends), and
    an emulator that has been told to stop is not an emulator that has
    stopped: the next row's first jobs opened images the last row's
    stragglers still had, and failed with "Failed to get write lock"
    -- four tests red for a reason that had nothing to do with them.

    Waiting politely is not enough. An emulator whose harness was killed
    on the clock was never told to stop at all, and waits for a console
    that will never be read again; it is still there after any limit. So
    the limit is where asking ends and killing begins: between two rows
    nothing in this tree is meant to be running, so anything that is, is
    a straggler, and the row that comes next is entitled to a clean
    tree. (Without this the whole full-matrix row came back red: the
    first job of 140 failed on an image the row before had left open,
    and sixty-six of the failures were that same lock.)"""
    for _ in range(limit):
        if quiet():
            return True
        time.sleep(1)
    print("  (emulators still running after %ds; killing them)" % limit,
          flush=True)
    for sig in ("-TERM", "-KILL"):
        subprocess.run(["pkill", sig, "-f", EMU],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(10):
            if quiet():
                return True
            time.sleep(1)
    print("  (emulators survived SIGKILL; going on anyway)", flush=True)
    return False


def finished(path, resume):
    """the numbers of a row that has already run, or None.

    The build box this campaign runs on resets itself under load -- six
    rows of ten, and then the machine was gone -- and a row costs four
    minutes that nobody should pay twice. Each row writes its JSON as it
    finishes, so the ones already there are the ones already done.

    Believing them is opt-in (--resume) on purpose: a JSON left over from
    an older tree would otherwise walk into a published table and read as
    a result of this one."""
    if not resume:
        return None
    try:
        with open(path) as f:
            r = json.load(f)
    except (OSError, ValueError):
        return None
    return r if all(k in r for k in ("jobs", "failed", "seconds")) else None


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
        done = finished(js, args.resume)
        if done:
            print("+ %s: kept from %s (%d tests, %d failed)"
                  % (name, os.path.basename(js), done["jobs"], done["failed"]),
                  flush=True)
            rows.append((name, why, done["jobs"], done["failed"],
                         done["seconds"]))
            continue
        cmd = [sys.executable, "tools/qrun.py", "--no-build", "--tier", "quick",
               "--json", js, "--progress", js + ".progress"]
        if args.resume:
            # a row cut in half resumes inside itself, too: the progress
            # file is the row's own, so one row can never read another's.
            cmd += ["--resume"]
        if args.capacity:
            cmd += ["--capacity", str(args.capacity)]
        if axis:
            cmd += ["--axis", axis]
        if "hyp=0" in axis:
            for s in NO_HYP_SUITES:
                cmd += ["-s", s]
        t0 = time.time()
        wait_quiet()
        run(cmd)
        # each row's console logs kept under its own name: they are
        # written to qlogs/NAME.0.out, and the next row writes over them,
        # so a failure three rows back had nothing left to look at.
        keep = os.path.join(HERE, "qlogs-%s" % name)
        subprocess.run(["rm", "-rf", keep])
        subprocess.run(["cp", "-a", os.path.join(HERE, "qlogs"), keep])
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
        r = finished(js, args.resume)
        if r:
            print("+ full matrix: kept from full.json (%d tests, %d failed)"
                  % (r["jobs"], r["failed"]), flush=True)
        else:
            cmd = [sys.executable, "tools/qrun.py", "--no-build", "--json", js,
                   "--progress", js + ".progress"]
            if args.resume:
                cmd += ["--resume"]
            if args.capacity:
                cmd += ["--capacity", str(args.capacity)]
            wait_quiet()
            run(cmd)
            r = finished(js, True)
        if r:
            rows.append(("full matrix", "every test, every controller",
                         r["jobs"], r["failed"], r["seconds"]))

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
    ap.add_argument("--resume", action="store_true",
                    help="keep the rows whose JSON is already in bench/campaign")
    ap.add_argument("--capacity", type=int, default=0, metavar="HARTS",
                    help="how many harts' worth of jobs at a time (qrun's "
                         "default is five eighths of the cores; a smaller "
                         "number is a cooler machine)")
    return campaign(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
