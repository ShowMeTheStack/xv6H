#
# process tests for test-hyp.py: there is no process table. thousands of
# processes at once, a hundred thousand on a big machine, pids that wrap
# around a small pid space, and everything reaped and returned.
#
import re, time
import test_hyp_guest as g


def run(q, line, timeout=60):
    m = q.mark()
    q.cmd_(line, timeout=timeout)
    return q.output_since(m)


def storm(q, n, timeout):
    out = run(q, "procstorm %d" % n, timeout)
    m = re.search(r"made (\d+) failed (\d+) maxpid (\d+) pid_max (\d+) procs (\d+)->(\d+) free (\d+)->(\d+)", out)
    assert m, out
    made, failed, maxpid, pidmax, p0, p1, f0, f1 = map(int, m.groups())
    assert made == n and failed == 0, out
    assert p1 == p0, out                 # every child reaped
    assert f1 >= f0 - 64, out            # and its memory back
    alive = re.search(r"procstorm: (\d+) alive \(kernel counts (\d+)\)", out)
    assert alive and int(alive.group(2)) >= n, out
    return out


def t_proc_8000(QEMU, intc, cpus, log):
    """eight thousand processes at once on a 1 GiB host (a process costs
    about 30 pages until copy-on-write fork arrives: stack, trapframe,
    vector state, seven page-table pages in Sv48, its copied user pages)."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        storm(q, 8000, 900)
        assert "ALL TESTS PASSED" in run(q, "usertests manyprocs", 300)
    finally:
        q.close()


def t_proc_100k(QEMU, intc, cpus, log):
    """a hundred thousand processes at once on 16 GiB."""
    q = QEMU(intc=intc, cpus=cpus, mem="16G", log=log)
    q.expect(r"\$ ", 120)
    try:
        t0 = time.time()
        storm(q, 100000, 3600)
        print("    100000 processes: %.0fs" % (time.time() - t0))
    finally:
        q.close()


def t_proc_pidwrap(QEMU, intc, cpus, log):
    """pidmax=300: pids wrap around and are reused only when free."""
    q = QEMU(intc=intc, cpus=cpus, log=log, extra_env={"APPEND": "pidmax=300"})
    q.expect(r"\$ ", 120)
    try:
        out = run(q, "procstorm -s 2000", 300)
        m = re.search(r"made (\d+) failed (\d+) maxpid (\d+) pid_max (\d+)", out)
        assert m and int(m.group(1)) == 2000 and int(m.group(2)) == 0, out
        assert int(m.group(3)) < 300 and int(m.group(4)) == 300, out
        # more processes than pids: the extra forks fail cleanly, the rest work
        out = run(q, "procstorm 400", 300)
        m = re.search(r"made (\d+) failed (\d+)", out)
        assert m and 250 <= int(m.group(1)) < 300 and int(m.group(2)) == 1, out
        assert "ALL TESTS PASSED" in run(q, "usertests forktest", 120)
    finally:
        q.close()


def tests(modes, cpus, log, QEMU):
    m = modes[0]
    return [
        ("proc-8000", lambda: t_proc_8000(QEMU, m, cpus, log)),
        ("proc-pidwrap", lambda: t_proc_pidwrap(QEMU, m, cpus, log)),
        ("proc-100k", lambda: t_proc_100k(QEMU, m, cpus, log)),
    ]
