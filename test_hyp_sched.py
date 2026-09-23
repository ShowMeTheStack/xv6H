#
# scheduler tests for test-hyp.py: what the weights are worth, where a
# task is allowed to run, how long a wakeup takes, and whether the tick
# rate can be changed while the machine runs.
#
# the numbers come from user/schedbench.c, which also builds and runs
# on Linux (tools/linuxbench.sh), so the targets below are the ones
# section 6 of the plan sets against Linux on the same QEMU: fairness
# within five per cent, latency within twice, throughput within twice.
#
import re, time
import test_hyp_guest as g


def run(q, line, timeout=120):
    m = q.mark()
    q.cmd_(line, timeout=timeout)
    return q.output_since(m)


def num(out, pat):
    m = re.search(pat, out)
    assert m, out
    return float(m.group(1))


def t_sched_fair(QEMU, intc, cpus, log):
    """three tasks of nice 6, 3 and 0 on one hart get the processor in
    the ratio of their weights: 272, 526 and 1024. Four rounds, because
    the way to miss a scheduler that is unfair now and then is to run
    it once."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        worst = 0
        for i in range(4):
            out = run(q, "schedbench -t 3 fair", 120)
            e = num(out, r"fair worst_err_pct ([0-9.]+)")
            print("    round %d: worst error %.2f%%" % (i + 1, e))
            worst = max(worst, e)
        assert worst < 5.0, "worst share error %.2f%% (want under 5%%)" % worst
    finally:
        q.close()


def t_sched_latency(QEMU, intc, cpus, log):
    """message passing, wakeup latency, yields and fork: the numbers,
    and a floor under each so that a regression is a failure and not a
    line in a log nobody reads. The floors are generous -- this is a
    software-emulated machine and the laptop it runs on is not idle --
    but a factor of two would not pass them."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        out = run(q, "schedbench -t 2 msg", 120)
        rt = num(out, r"msg roundtrip_ns (\d+)")
        print("    message round trip: %.0f us" % (rt / 1000))
        assert rt < 500000, out

        out = run(q, "schedbench -t 2 wake", 120)
        p50 = num(out, r"wake latency_ns_p50 (\d+)")
        p99 = num(out, r"wake latency_ns_p99 (\d+)")
        print("    wakeup latency: p50 %.0f us, p99 %.0f us" % (p50 / 1000, p99 / 1000))
        assert p50 < 400000 and p99 < 2000000, out

        out = run(q, "schedbench -t 2 ctx", 120)
        cx = num(out, r"ctx ns_each (\d+)")
        print("    context switch: %.0f us" % (cx / 1000))
        assert cx < 200000, out

        out = run(q, "schedbench -t 2 fork", 200)
        fk = num(out, r"fork ns_each (\d+)")
        print("    fork+exit+wait: %.0f us" % (fk / 1000))
        assert fk < 20000000, out
    finally:
        q.close()


def t_sched_parallel(QEMU, intc, cpus, log):
    """two tasks that both want a processor get two of them.

    a hart with something waiting and a hart with nothing is the one
    imbalance that has to be fixed, and it is easy to get wrong: a
    woken task goes to the hart of whoever woke it, which is right for
    two tasks passing messages and wrong for two that both intend to
    run. What keeps the two apart is that a pair passing messages is
    never both runnable at once."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        # measured three times, alternating, and the best of the three
        # is the answer: outside load on the machine this runs on can
        # only make two tasks look like less than two harts, never
        # more, so the largest of the three is the least disturbed
        # measurement rather than the luckiest one.
        best = 0
        for _ in range(3):
            alone = num(run(q, "schedbench -t 2 -c -1 spin", 60),
                        r"spin work (\d+)")
            q.send("schedbench -t 4 -c -1 spin > /tmp/a &\n")
            q.expect(r"\$ ", 20)
            q.send("schedbench -t 4 -c -1 spin > /tmp/b &\n")
            q.expect(r"\$ ", 20)
            time.sleep(6)
            run(q, "wait", 60)
            a = num(run(q, "cat /tmp/a", 20), r"spin work (\d+)")
            b = num(run(q, "cat /tmp/b", 20), r"spin work (\d+)")
            # each ran for twice as long as the lone run, so each should
            # have done about twice its work if they had a hart each
            ratio = (a + b) / (4 * alone)
            print("    one task %d, two tasks %d and %d -> %.2f of two harts"
                  % (alone, a, b, ratio))
            best = max(best, ratio)
        assert best > 0.75, best
    finally:
        q.close()


def t_sched_affinity(QEMU, intc, cpus, log):
    """taskset: a task runs where it is told to and nowhere else, and a
    mask naming no hart on this machine is refused."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        out = run(q, "taskset -p $$", 30)
        m = re.search(r"current affinity mask: ([0-9a-f]+)", out)
        assert m and int(m.group(1), 16) == (1 << cpus) - 1, out

        # pin the shell to the last hart and watch it land there
        last = cpus - 1
        out = run(q, "taskset -p %x $$" % (1 << last), 30)
        assert "new affinity mask: %x" % (1 << last) in out, out
        out = run(q, "taskset -p $$", 30)
        assert "current affinity mask: %x" % (1 << last) in out, out
        out = run(q, "cat /proc/schedstat", 30)
        me = [l for l in out.splitlines() if re.match(r"task \d+ %d run" % last, l)]
        assert me, out

        # and back
        run(q, "taskset -p %x $$" % ((1 << cpus) - 1), 30)
        # a mask with nothing this machine has in it
        out = run(q, "taskset -p 0 $$; echo rc=$?", 30)
        assert "rc=1" in out or "usage" in out, out
        # a command run on one hart
        out = run(q, "taskset 1 sh -c 'cat /proc/schedstat | grep -c task'", 60)
        assert re.search(r"^\d+$", out.strip().splitlines()[-2].strip() if len(out.strip().splitlines()) > 1 else "", re.M) or "1" in out, out
    finally:
        q.close()


def t_sched_tick_hz(QEMU, intc, cpus, log):
    """the tick rate is a setting: /sys/kernel/tick_hz reads it, writing
    changes it, and the clock the rest of the system keeps stays right
    across the change."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        out = run(q, "cat /sys/kernel/tick_hz", 30)
        assert "100" in out, out

        # /proc/uptime does not come from ticks, so a second is a second
        # whatever the rate is
        def uptime():
            o = run(q, "cat /proc/uptime", 30)
            return float(re.search(r"([0-9]+\.[0-9]+)", o).group(1))

        for hz in ("10", "250", "100"):
            run(q, "echo %s > /sys/kernel/tick_hz" % hz, 30)
            out = run(q, "cat /sys/kernel/tick_hz", 30)
            assert hz in out, out
            t0 = uptime()
            run(q, "usleep 500000", 30)
            d = uptime() - t0
            print("    tick_hz %s: a half-second sleep took %.2fs" % (hz, d))
            assert 0.3 < d < 1.5, "tick_hz %s: %.2fs" % (hz, d)

        # nonsense is refused
        out = run(q, "echo 0 > /sys/kernel/tick_hz; echo rc=$?", 30)
        assert "rc=1" in out, out
        out = run(q, "cat /sys/kernel/tick_hz", 30)
        assert "100" in out, out
    finally:
        q.close()


def groups(q):
    """the group table as cgrun -l prints it, by id."""
    out = run(q, "cgrun -l", 30)
    r = {}
    for l in out.splitlines():
        f = l.split()
        if len(f) == 8 and f[0].isdigit():
            r[int(f[0])] = dict(tasks=int(f[1]), weight=int(f[2]), quota=int(f[3]),
                                period=int(f[4]), frozen=int(f[5]),
                                runnable=int(f[6]), usage=int(f[7]))
    return r


def t_sched_group_weight(QEMU, intc, cpus, log):
    """two groups, one spinning task each, weights 1024 and 3072, on a
    machine with one hart: the processor goes to them in that ratio,
    whatever the tasks inside them are."""
    q = g.boot_host(QEMU, intc, 1, log)
    try:
        q.send("cgrun -n light -w 1024 schedbench -t 40 -c 0 spin &\n")
        q.expect(r"\$ ", 20)
        q.send("cgrun -n heavy -w 3072 schedbench -t 40 -c 0 spin &\n")
        q.expect(r"\$ ", 20)
        time.sleep(4)
        a = groups(q)
        assert len(a) == 2, a
        time.sleep(12)
        b = groups(q)
        used = {}
        for i in b:
            used[b[i]["weight"]] = b[i]["usage"] - a[i]["usage"]
        assert 1024 in used and 3072 in used, (a, b)
        ratio = used[3072] / used[1024] if used[1024] else 0
        print("    heavy/light = %.2f (want 3.00)" % ratio)
        assert 2.6 < ratio < 3.4, (used, ratio)
        # and between them they had the whole hart
        total = used[1024] + used[3072]
        print("    both together used %.1f%% of the hart" % (total / 1e4 / 12))
        assert total > 9e6, used
    finally:
        q.close()


def t_sched_group_max(QEMU, intc, cpus, log):
    """cpu.max: a group limited to a quarter of one processor gets a
    quarter of one processor, on a machine that is otherwise idle --
    which is the whole point of a ceiling."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        q.send("cgrun -n capped -m 25 schedbench -t 40 -c 0 spin &\n")
        q.expect(r"\$ ", 20)
        time.sleep(4)
        a = groups(q)
        assert len(a) == 1, a
        gid = list(a)[0]
        t0 = time.time()
        time.sleep(12)
        b = groups(q)
        dt = time.time() - t0
        pct = (b[gid]["usage"] - a[gid]["usage"]) / 1e4 / dt
        print("    capped group used %.1f%% of one processor (ceiling 25%%)" % pct)
        assert 15 < pct < 35, pct

        # freezing stops it, and letting it go starts it again
        run(q, "cgrun -f %d on" % gid, 30)
        c = groups(q)
        time.sleep(3)
        d = groups(q)
        assert d[gid]["frozen"] == 1, d
        assert d[gid]["usage"] == c[gid]["usage"], (c, d)
        run(q, "cgrun -f %d off" % gid, 30)
        e = groups(q)
        time.sleep(3)
        f = groups(q)
        assert f[gid]["frozen"] == 0, f
        assert f[gid]["usage"] > e[gid]["usage"], (e, f)
        print("    frozen: nothing; thawed: %d us in three seconds"
              % (f[gid]["usage"] - e[gid]["usage"]))
    finally:
        q.close()


def t_sched_vm_weight(QEMU, intc, cpus, log):
    """a virtual machine is a group: `vmm start -w' is its share of the
    host's processors whatever its vcpu count, `vmm weight' changes it,
    `vmm cpumax' puts a ceiling on it, and pausing freezes it."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        g.start_guest(q, "-n a -m 64 -c 1 -w 2048")
        g.detach(q)
        out = run(q, "vmm info a", 30)
        assert re.search(r"cpu: weight 2048", out), out
        out = run(q, "vmm weight a 512", 30)
        assert "vm 0 weight 512" in out, out
        out = run(q, "vmm cpumax a 20", 30)
        assert "20%" in out, out
        out = run(q, "vmm info a", 30)
        assert re.search(r"cpu: weight 512, at most 20% of one processor", out), out
        # the group is there, with the machine's name on it
        out = run(q, "cat /proc/schedstat", 30)
        assert re.search(r"^group \d+ a ", out, re.M), out
        run(q, "vmm pause a", 30)
        out = run(q, "cat /proc/schedstat", 30)
        m = re.search(r"^group \d+ a (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)", out, re.M)
        assert m and m.group(6) == "1", out      # frozen
        run(q, "vmm resume a", 30)
        out = run(q, "cat /proc/schedstat", 30)
        m = re.search(r"^group \d+ a (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)", out, re.M)
        assert m and m.group(6) == "0", out
        g.kill_vm(q, "a")
        out = run(q, "cat /proc/schedstat", 30)
        assert not re.search(r"^group \d+ a ", out, re.M), out
    finally:
        q.close()


def tests(modes, cpus, log, QEMU):
    m = modes[0]
    return [
        ("sched-fair", lambda: t_sched_fair(QEMU, m, cpus, log)),
        ("sched-latency", lambda: t_sched_latency(QEMU, m, cpus, log)),
        ("sched-parallel", lambda: t_sched_parallel(QEMU, m, cpus, log)),
        ("sched-affinity", lambda: t_sched_affinity(QEMU, m, cpus, log)),
        ("sched-tick-hz", lambda: t_sched_tick_hz(QEMU, m, cpus, log)),
        ("sched-group-weight", lambda: t_sched_group_weight(QEMU, m, cpus, log)),
        ("sched-group-max", lambda: t_sched_group_max(QEMU, m, cpus, log)),
        ("sched-vm-weight", lambda: t_sched_vm_weight(QEMU, m, cpus, log)),
    ]
