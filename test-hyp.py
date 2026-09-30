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
#   ./test-hyp.py sched             # the scheduler: fairness, latency, affinity, the tick rate
#   ./test-hyp.py all               # everything
#
import argparse, os, re, select, shlex, shutil, subprocess, sys, time

sys.stdout.reconfigure(line_buffering=True)

HERE = os.path.dirname(os.path.abspath(__file__))
MODES = ["plic", "aplic", "imsic"]

# every wait in this file is a hang detector, not a stopwatch: the
# numbers are generous multiples of what a step takes on an idle
# machine. When several tests share a machine -- tools/qrun.py runs a
# dozen at once -- the multiple shrinks, and a test starts failing for
# want of a processor rather than for a reason. The runner says how
# hard it is pushing and the waits are scaled to match; the hang
# detection is unchanged, since a hang still never finishes.
TIMEOUT_SCALE = float(os.environ.get("TEST_TIMEOUT_SCALE", "1"))


# The axis this run is on (§11.1): what machine every test gets, unless
# the test asks for something else on purpose. `--axis mem=16G,sv=39'
# runs the same tests on a different machine, which is what a campaign
# is made of. Empty means the defaults below.
AXIS = {}


def axis_default(name, given, default):
    """what a test asked for, else what the axis says, else the default."""
    if given is not None:
        return given
    return AXIS.get(name, default)


HOST_APPEND = ""   # --append: words for every host's command line


class QEMU(object):
    live = []     # the hosts the running test has started (timeline_report)
    def __init__(self, intc=None, cpus=None, mem=None, aia_guests=2, hyp=None, vec=None,
                 vlen=None, sv=None, log=None,
                 extra_env=None, keep_fs=False, fsimg=None, cboz=None):
        intc = axis_default("intc", intc, "plic")
        cpus = int(axis_default("cpus", cpus, 3))
        mem = axis_default("mem", mem, "1G")
        hyp = int(axis_default("hyp", hyp, 1))
        vec = int(axis_default("vec", vec, 1))
        vlen = int(axis_default("vlen", vlen, 128))
        sv = int(axis_default("sv", sv, 48))
        cboz = int(axis_default("cboz", cboz, 64))
        # every run starts from a pristine copy of the host file system
        # (TEST_FSIMG names the copy, so that two runs can go in
        # parallel). keep_fs starts from the copy the last run left,
        # which is what a test of what survives a reboot needs.
        # one image per machine: a test that starts two of them (E11's
        # migration) says which, because one image opened twice is one
        # file system with two kernels writing it.
        fsimg = fsimg or os.environ.get("TEST_FSIMG", "test-fs.img")
        env = dict(os.environ, INTC=intc, CPUS=str(cpus), MEM=mem, AIA_GUESTS=str(aia_guests),
                   HYP=str(hyp), VEC=str(vec), VLEN=str(vlen), SV=str(sv), FSIMG=fsimg,
                   CBOZ=str(cboz))
        if extra_env:
            env.update(extra_env)
        if HOST_APPEND:
            # words for every host's command line (--append), on top of a test's own
            env["APPEND"] = (env.get("APPEND", "") + " " + HOST_APPEND).strip()
        if not keep_fs or not os.path.exists(os.path.join(HERE, fsimg)):
            shutil.copyfile(os.path.join(HERE, "fs.img"), os.path.join(HERE, fsimg))
        cmd = subprocess.run(["make", "-s", "--no-print-directory", "qemucmd"], cwd=HERE,
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             encoding="utf8", check=True).stdout.strip().split("\n")[-1]
        self.cmd = cmd
        self.log = log
        # Three things are true of every machine this harness starts, and
        # they are checked here rather than believed, because they are the
        # whole of what keeps the emulated network inside the emulator:
        #
        #   - the network is qemu's user-mode NAT and nothing else. No
        #     hostfwd, no guestfwd, no tap, no bridge, no vde, no socket
        #     and no -net.
        #   - so nothing on the emulated network can open a connection to
        #     this laptop or to anything on its network: outbound through
        #     the NAT is all there is.
        #   - and a port a machine publishes exists on the emulated
        #     network only. Nothing listens on the laptop for it, which
        #     is checked after the boot (assert_no_listeners).
        for bad in ("hostfwd", "guestfwd", "-net ", "type=tap", "ifname=",
                    "type=bridge", "type=vde", "type=socket", "type=dgram"):
            assert bad not in cmd, "qemu: %s in %s" % (bad, cmd)
        # one exception, and it is the whole of it: two hosts joined to
        # each other for a migration (E11) are joined by a *unix* socket
        # on this laptop's file system -- a path, not a port. Nothing
        # listens on the network, nothing outside the two emulated
        # machines can reach either end, and a TCP form of the same
        # option would be refused here like everything else above.
        for dev in re.findall(r"-netdev (\S+)", cmd):
            if dev.startswith("user,"):
                continue
            assert dev.startswith("stream,") and "addr.type=unix" in dev, cmd
            assert "addr.host=" not in dev and "addr.port=" not in dev, cmd
        self.proc = subprocess.Popen(shlex.split(cmd), cwd=HERE, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.buf = b""
        QEMU.live.append(self)
        self.consumed = 0
        self.gone = False   # the machine's console has closed

    # what the emulator has open, from the laptop's point of view: no
    # listening socket at all. A published port lives on the emulated
    # network; if one ever appeared here, it would be on the laptop.
    def assert_no_listeners(self):
        try:
            # -a, or lsof takes the selectors as alternatives and reports
            # every listening socket on the laptop rather than this
            # process's (which is how this check first "failed")
            out = subprocess.run(["lsof", "-nP", "-a", "-p", str(self.proc.pid),
                                  "-iTCP", "-sTCP:LISTEN"], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, encoding="utf8",
                                 timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            return          # no lsof: the command line check above still holds
        listening = [l for l in out.splitlines() if "LISTEN" in l]
        assert not listening, "qemu is listening on the laptop: %s" % listening

    def _pump(self, timeout):
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r:
            return False
        d = os.read(self.proc.stdout.fileno(), 65536)
        if not d:
            self.gone = True
            return False
        self.buf += d
        if self.log:
            self.log.write(d.decode(errors="replace"))
            self.log.flush()
        return True

    # the host and its guests write to one console, so a line the host
    # prints about a guest ("hyp: vm 0 vcpu 0: store to unmapped gpa
    # ...") can land in the middle of a line the guest is printing:
    #
    #     badguest: helhyp: vm 0 vcpu 0: store to unmapped ...
    #     lo from a hostile guest
    #
    # A pattern that spans the join then never matches. When the plain
    # search fails, the search is tried again on a copy with the host's
    # own lines taken out, which puts the guest's line back together.
    HOSTLINE = re.compile(rb"hyp: [^\n]*\n")

    def expect(self, pattern, timeout=60):
        """wait for a regex in output produced since the last expect()."""
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern, re.S)
        end = time.time() + timeout * TIMEOUT_SCALE
        while True:
            m = rx.search(self.buf, self.consumed)
            if m:
                self.consumed = m.end()
                return m
            rest = self.buf[self.consumed:]
            if b"hyp: " in rest:
                m = rx.search(self.HOSTLINE.sub(b"", rest))
                if m:
                    # the match spanned a host line; everything up to
                    # here has been seen, so start the next wait from
                    # the end of what has arrived.
                    self.consumed = len(self.buf)
                    return m
            if self.gone:
                rc = self.proc.poll()
                tail = self.buf[-3000:].decode(errors="replace")
                raise TimeoutError("qemu exited (status %s) while waiting for %r;"
                                   " tail:\n%s" % (rc, pattern, tail))
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


def run_cmd(q, line, timeout=60):
    """send one command and return what it printed. every command the
    tests send is short enough that the line editor does not have to
    redraw it, which is what keeps the prompt a reliable marker.

    a command that is too long is caught here rather than three
    commands later: the editor redraws the line, the escape sequence
    lands in the output, and everything after that is out of step with
    the prompts the tests are waiting for."""
    if len(line) > 74:
        raise AssertionError("command is %d characters, which the line editor will redraw: %r"
                             % (len(line), line))
    m = q.mark()
    q.cmd_(line, timeout=timeout)
    out = q.output_since(m)
    if "\x1b[" in out:
        raise AssertionError("the line editor redrew %r; the harness is out of step" % line)
    return out


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
                           ("sh /shtest", r"shtest: (\d+) checks, 0 failures"),
                           ("sh /texttest", r"texttest: (\d+) checks, 0 failures"),
                           ("sh /archtest", r"archtest: (\d+) checks, 0 failures"),
                           ("sh /filetest", r"filetest: (\d+) checks, 0 failures"),
                           ("nettest", r"nettest: (\d+) checks, 0 failures"),
                           ("sh /calctest", r"calctest: (\d+) checks, 0 failures"),
                           ("sh /systest", r"systest: (\d+) checks, 0 failures")):
            m = q.mark()
            q.send(prog + "\n")
            q.expect(r"\d+ (?:checks|tests), \d+ failures?\r?\n|panic", 600)
            out = q.output_since(m)
            assert re.search(want, out), out[-3000:]
            print("    %s: %s" % (prog, re.search(want, out).group(0)))
            q.expect(r"\$ ", 30)
    finally:
        q.close()


def t_blockdev(log):
    """/dev is a file system the kernel fills in, a disk can be opened
    as a file, and the ioctls every tool uses to ask a device about
    itself answer."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        out = run_cmd(q, "ls /dev")
        for want in ("console", "null", "zero", "random", "vda"):
            assert re.search(r"(?m)^%s\s*$" % want, out), (want, out)

        out = run_cmd(q, "blkid /dev/vda")
        m = re.search(r'/dev/vda: UUID="([0-9a-f-]{36})" TYPE="ext4"', out)
        assert m, out
        print("    /dev/vda uuid %s" % m.group(1))

        out = run_cmd(q, "blockdev --getsize64 /dev/vda")
        m = re.search(r"(?m)^(\d+)\s*$", out)
        assert m and int(m.group(1)) >= 64 * 1024 * 1024, out
        size = int(m.group(1))
        out = run_cmd(q, "blockdev --getss /dev/vda")
        assert re.search(r"(?m)^512\s*$", out), out
        out = run_cmd(q, "blockdev --getbsz /dev/vda")
        assert re.search(r"(?m)^4096\s*$", out), out
        out = run_cmd(q, "blockdev --getro /dev/vda")
        assert re.search(r"(?m)^0\s*$", out), out
        run_cmd(q, "blockdev --flushbufs /dev/vda")
        run_cmd(q, "blockdev --rereadpt /dev/vda")

        # the device reads as a file, and what comes off it is the
        # superblock the file system was made with
        out = run_cmd(q, "dd if=/dev/vda of=/sb bs=1024 count=2")
        assert "2048 bytes copied" in out, out
        out = run_cmd(q, "ls -l /sb")
        assert re.search(r"\b2048\b", out), out
        out = run_cmd(q, "blkid /sb")
        assert 'TYPE="ext4"' in out, out
        run_cmd(q, "rm /sb")
        print("    %d bytes, read through /dev/vda" % size)
    finally:
        q.close()


def t_loopdev(log):
    """a file made to look like a disk, and a partition table written on
    one. the second disk is a small ext4 image made by the same mkext4
    that makes the root, and everything after that happens inside."""
    img = os.path.join(HERE, "test-loop.img")
    subprocess.run([os.path.join(HERE, "mkfs", "mkext4"), "-b", "4096", "-j", "1024", "-L",
                    "looptest", img], check=True, stdout=subprocess.DEVNULL)
    q = QEMU(intc="plic", cpus=3, log=log, extra_env={"EXTRADISK": img})
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # a copy of the image as an ordinary file, straight off the disk
        out = run_cmd(q, "dd if=/dev/vdb of=/img bs=65536", 120)
        assert "16777216 bytes copied" in out, out

        # attached to a loop device, it is a disk with a file system
        run_cmd(q, "losetup /dev/loop0 /img")
        out = run_cmd(q, "blkid /dev/loop0")
        assert 'LABEL="looptest"' in out and 'TYPE="ext4"' in out, out
        run_cmd(q, "mkdir /m")
        out = run_cmd(q, "mount -t ext4 /dev/loop0 /m", 30)
        assert "on /m type ext4" in out, out
        run_cmd(q, "echo through-loop > /m/hello")
        out = run_cmd(q, "cat /m/hello")
        assert "through-loop" in out, out
        run_cmd(q, "sync", 60)
        run_cmd(q, "umount /m", 60)
        run_cmd(q, "losetup -d /dev/loop0")

        # a raw file, partitioned, with the image written onto the first
        # partition: the whole chain, on a loop device
        out = run_cmd(q, "dd if=/dev/zero of=/raw bs=65536 count=336", 300)
        assert "22020096 bytes copied" in out, out
        run_cmd(q, "losetup /dev/loop1 /raw")
        run_cmd(q, "echo 'start=2048, size=40000' > /p.txt")
        out = run_cmd(q, "sfdisk /dev/loop1 < /p.txt", 60)
        assert "1 partition written" in out, out
        out = run_cmd(q, "sfdisk -l /dev/loop1")
        assert "Disklabel type: dos" in out and "2048" in out, out
        out = run_cmd(q, "ls /dev")
        assert re.search(r"(?m)^loop1p1\s*$", out), out
        out = run_cmd(q, "dd if=/img of=/dev/loop1p1 bs=65536", 300)
        assert "16777216 bytes copied" in out, out
        out = run_cmd(q, "mount -t ext4 /dev/loop1p1 /m", 30)
        assert "on /m type ext4" in out, out
        out = run_cmd(q, "cat /m/hello")
        assert "through-loop" in out, out
        run_cmd(q, "sync", 60)
        run_cmd(q, "umount /m", 60)
        run_cmd(q, "losetup -d /dev/loop1")
        print("    a file system on a partition of a loop device, read back")
    finally:
        q.close()
        try:
            os.unlink(img)
        except OSError:
            pass


def t_init(log):
    """sysvinit: /etc/inittab, rcS, the runlevel scripts, service,
    update-rc.d and start-stop-daemon."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        m = q.mark()
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        boot = q.output_since(m)
        # what init did on the way up, in order
        assert "rcS: starting" in boot and "rcS: done" in boot, boot[-800:]
        assert "Starting syslogd" in boot and "Starting klogd" in boot, boot[-800:]

        # the daemons the runlevel scripts started, and nothing else
        out = run_cmd(q, "ps -ef")
        assert "syslogd" in out and "klogd" in out, out
        # init runs a plain command directly rather than under a shell
        assert len([l for l in out.split("\n") if l.rstrip().endswith(" sh")]) == 1, out

        # a service, stopped and started again
        out = run_cmd(q, "service syslogd status", 30)
        assert "is running" in out, out
        run_cmd(q, "service syslogd stop", 30)
        out = run_cmd(q, "service syslogd status", 30)
        assert "not running" in out, out
        run_cmd(q, "service syslogd start", 30)
        out = run_cmd(q, "service syslogd status", 30)
        assert "is running" in out, out

        # the runlevel links, removed and made again
        run_cmd(q, "update-rc.d klogd remove", 30)
        out = run_cmd(q, "ls /etc/rc.d/rc2.d")
        assert "klogd" not in out.split(), out
        out = run_cmd(q, "update-rc.d klogd defaults", 30)
        assert "S20klogd" in out, out   # after syslogd, which it requires
        out = run_cmd(q, "ls /etc/rc.d/rc2.d")
        assert "S20klogd" in out.split(), out

        # start-stop-daemon will not start a second one
        out = run_cmd(q, "start-stop-daemon --start --exec /bin/klogd --background", 30)
        assert "already running" in out, out
        out = run_cmd(q, "start-stop-daemon --stop --name klogd --retry 2", 30)
        out = run_cmd(q, "pgrep klogd")
        assert out.strip().endswith("$"), out

        # crond: a job that runs every minute, and says so in the log
        out = run_cmd(q, "service crond status", 30)
        assert "is running" in out, out
        run_cmd(q, "echo '* * * * * echo cron-ran-this' > /ct")
        out = run_cmd(q, "crontab /ct", 30)
        assert "installed" in out, out
        out = run_cmd(q, "crontab -l", 30)
        assert "cron-ran-this" in out, out
        run_cmd(q, "sleep 65", 120)
        out = run_cmd(q, "grep cron-ran-this /var/log/messages", 30)
        assert "CMD (echo cron-ran-this)" in out, out
        assert re.search(r"(?m)^cron-ran-this$", out), out  # what the job said
        run_cmd(q, "crontab -r", 30)
        out = run_cmd(q, "crontab -l", 30)
        assert "no table" in out, out
        # the networking service reads /etc/network/interfaces
        out = run_cmd(q, "service networking status", 30)
        assert "eth0" in out or "10.0.2" in out, out

        # httpd: the example service, asked for a file over loopback
        out = run_cmd(q, "service httpd status", 30)
        assert "is running" in out, out
        out = run_cmd(q, "wget -q -O - http://127.0.0.1/hello.txt", 30)
        assert "hello from httpd" in out, out
        # a path that tries to leave the directory is refused, not resolved
        out = run_cmd(q, "wget -q -O - http://127.0.0.1/../etc/passwd", 30)
        assert "root:x:0:0" not in out, out
        out = run_cmd(q, "grep 'GET /hello.txt' /var/log/messages", 30)
        assert "200" in out, out

        print("    inittab, rcS, the runlevel scripts, service, start-stop-daemon, crond")
    finally:
        q.close()


def t_namespaces(log):
    """namespaces: the machine's name is one thing in one namespace and
    another in another, and what a namespace does stays in it."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        out = run_cmd(q, "hostname")
        assert "xv6h" in out, out

        # a command in a namespace of its own may call the machine
        # whatever it likes
        out = run_cmd(q, "unshare -u --hostname inside hostname")
        assert "inside" in out, out
        out = run_cmd(q, "hostname")
        assert "xv6h" in out and "inside" not in out, out

        # everything that reports the name reports the namespace's
        out = run_cmd(q, "unshare -u --hostname inside uname -n")
        assert "inside" in out, out
        out = run_cmd(q, "unshare -u --hostname inside cat /sys/kernel/hostname")
        assert "inside" in out, out
        out = run_cmd(q, "cat /sys/kernel/hostname")
        assert "xv6h" in out, out

        # a child is in its parent's namespace, and a change there is a
        # change for both of them and for nobody else
        out = run_cmd(q, "unshare -u --hostname inside sh -c 'hostname; "
                         "hostname deeper; hostname'")
        assert "inside" in out and "deeper" in out, out
        out = run_cmd(q, "hostname")
        assert "xv6h" in out, out

        # the namespace goes when the last process in it does, so the
        # pool does not run out: eight of them, one after another
        for i in range(8):
            out = run_cmd(q, "unshare -u --hostname n%d hostname" % i)
            assert "n%d" % i in out, out

        # and making one is privileged, until user namespaces
        out = run_cmd(q, "su user -c 'unshare -u --hostname nope hostname'", 30)
        assert "not permitted" in out.lower(), out
        out = run_cmd(q, "hostname")
        assert "xv6h" in out, out

        # a namespace has a name of its own under /proc, which is how one
        # is pointed at: two processes in the same namespace read the same
        # thing there and two in different ones do not
        out = run_cmd(q, "ls /proc/self/ns")
        assert "uts" in out, out
        root_id = run_cmd(q, "cat /proc/self/ns/uts")
        assert re.search(r"uts:\[\d+\]", root_id), root_id
        other = run_cmd(q, "unshare -u --hostname elsewhere cat /proc/self/ns/uts")
        assert re.search(r"uts:\[\d+\]", other), other
        assert other.split("uts:")[1] != root_id.split("uts:")[1], (root_id, other)

        # nsenter: a command run in a namespace somebody else is in. The
        # process to point at is one left running in a namespace of its
        # own.
        out = run_cmd(q, "unshare -u --hostname neighbour sleep 20 &", 20)
        # the shell says what it started; pgrep is asked as well, because
        # a process that has exec'd something else should still be found
        # by the name of what it is now
        m = re.search(r"\[\d+\]\s+(\d+)", out)
        assert m, out
        pid = m.group(1)
        out = run_cmd(q, "ps -ef | grep sleep")
        assert "sleep" in out, out
        # and it is called what it is running, not what started it
        out = run_cmd(q, "head -1 /proc/%s/status" % pid)
        assert "sleep" in out, out
        out = run_cmd(q, "pgrep sleep")
        assert pid in out, out
        out = run_cmd(q, "cat /proc/%s/ns/uts" % pid)
        assert re.search(r"uts:\[\d+\]", out), out
        out = run_cmd(q, "nsenter -t %s -u hostname" % pid, 30)
        assert "neighbour" in out, out
        # and the machine outside is still itself
        out = run_cmd(q, "hostname")
        assert "xv6h" in out, out
        # a name that nobody is in is not a namespace to enter
        out = run_cmd(q, "nsenter -t 31337 -u hostname", 30)
        assert "no such" in out.lower() or "not" in out.lower(), out
        run_cmd(q, "kill %s" % pid, 20)
        print("    the machine's name is the namespace's, and namespaces are"
              " named under /proc")
    finally:
        q.close()


def t_containers(log):
    """a container: its own root, its own name, its own numbering, its own
    identity, and limits around it -- built out of the namespaces and the
    control group, and none of it the machine's.

    (Commands are kept short and the long ones put in files: every line
    has to fit in the console's line editor.)"""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # a root for it: a few programs and the C library
        run_cmd(q, "mkdir -p /ct/bin /ct/lib", 20)
        for f in ("sh", "ls", "cat", "hostname", "id", "ps", "sleep",
                  "cgtest", "echo"):
            run_cmd(q, "cp /bin/%s /ct/bin/%s" % (f, f), 30)
        run_cmd(q, "cp /lib/libc.so /ct/lib/libc.so", 30)
        run_cmd(q, "cp /lib/ld.so /ct/lib/ld.so", 30)
        # cp does not carry the mode across, and a program nobody may
        # execute is a program the container cannot run -- rightly
        run_cmd(q, "chmod 755 /ct/bin/*", 30)
        run_cmd(q, "chmod 755 /ct/lib/*", 30)

        # what it should be able to say about itself
        run_cmd(q, "echo 'hostname' > /ct/probe")
        run_cmd(q, "echo 'echo pid=$$' >> /ct/probe")
        run_cmd(q, "echo 'echo uid=$(id -u)' >> /ct/probe")
        run_cmd(q, "echo 'ls /' >> /ct/probe")
        run_cmd(q, "echo 'cat /etc/passwd' >> /ct/probe")
        run_cmd(q, "echo 'echo in > /tmp/x; cat /tmp/x' >> /ct/probe")

        out = run_cmd(q, "vmm run -n c1 --rootfs /ct -h boxy /bin/sh /probe", 90)
        assert "boxy" in out, out
        assert "pid=1" in out, out            # the first process of its own numbering
        assert "uid=0" in out, out            # root, inside
        assert "bin" in out and "lib" in out, out
        assert "proc" in out and "tmp" in out, out   # what it mounted
        assert "etc" not in out.split(), out         # and not the machine's
        assert "cannot open" in out or "No such" in out, out  # /etc/passwd
        assert re.search(r"(?m)^in\s*$", out), out   # its own /tmp works

        # the machine is untouched: its name, and what it has mounted
        out = run_cmd(q, "hostname")
        assert "xv6h" in out and "boxy" not in out, out
        out = run_cmd(q, "mount | grep -c /ct")
        assert re.search(r"(?m)^0\s*$", out), out

        # and the file the container made is an ordinary user's out here
        out = run_cmd(q, "ls -l /ct/bin/sh | head -1")
        assert "sh" in out, out

        # its limits are the group's: eight processes and no more
        out = run_cmd(q, "vmm run -n c2 --rootfs /ct --pids 8 /bin/cgtest fork 40",
                      90)
        m = re.search(r"forks (\d+) of 40", out)
        assert m and int(m.group(1)) < 10, out

        # a container that is left running is listed, and can be stopped
        run_cmd(q, "echo 'sleep 60' > /ct/idle")
        out = run_cmd(q, "vmm run -d -n c3 --rootfs /ct /bin/sh /idle", 60)
        assert "container c3" in out, out
        out = run_cmd(q, "vmm ps", 30)
        assert "c3" in out, out
        out = run_cmd(q, "vmm stop c3", 30)
        assert "stopped" in out, out
        out = run_cmd(q, "vmm ps", 30)
        assert "c3" not in out, out

        # the machine is still itself
        out = run_cmd(q, "head -1 /etc/passwd")
        assert "root:x:0:0" in out, out
        out = run_cmd(q, "id -u")
        assert re.search(r"(?m)^0\s*$", out), out
        print("    a container is a process tree with its own everything")
    finally:
        q.close()


def t_sysfilter(log):
    """what a container may ask the kernel for.

    The namespaces bound what it can name and the control group bounds
    what it can take; this is the third fence -- what it can call. The
    profile is installed by the runtime as its last act before the
    command, is inherited, and cannot be widened. Checked from inside:
    every call on the list is refused, ordinary work is not, and then a
    few thousand calls with arguments nobody meant, to see whether the
    kernel or the machine minds."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # a root for it
        run_cmd(q, "mkdir -p /fz/bin /fz/lib", 20)
        for f in ("sh", "cat", "echo", "fuzzsys", "hostname", "ls"):
            run_cmd(q, "cp /bin/%s /fz/bin/%s" % (f, f), 30)
        run_cmd(q, "cp /lib/libc.so /fz/lib/libc.so", 30)
        run_cmd(q, "cp /lib/ld.so /fz/lib/ld.so", 30)
        run_cmd(q, "chmod 755 /fz/bin/* /fz/lib/*", 30)

        # every call the profile names is refused, from inside, and a
        # child of a filtered process is filtered too
        out = run_cmd(q, "vmm run -n f1 --rootfs /fz /bin/fuzzsys check", 120)
        assert "calls refused" in out, out
        assert "a child is filtered too" in out, out
        assert "check ok" in out, out
        assert "FAILED" not in out, out

        # the same calls through the programs a container would reach for
        run_cmd(q, "echo 'hostname other' > /fz/probe")
        run_cmd(q, "echo 'echo rc=$?' >> /fz/probe")
        run_cmd(q, "echo 'hostname' >> /fz/probe")
        out = run_cmd(q, "vmm run -n f2 --rootfs /fz -h boxy /bin/sh /probe", 90)
        assert "not permitted" in out.lower(), out   # sethostname refused
        assert "boxy" in out, out                    # and its name is its own
        assert "other" not in out, out

        # the machine's own business is not readable from inside it: the
        # audit ring and the list of running machines are privileged over
        # the machine, and root in a container is the other kind of root
        run_cmd(q, "echo 'cat /proc/audit' > /fz/peek")
        run_cmd(q, "echo 'cat /proc/vmm' >> /fz/peek")
        out = run_cmd(q, "vmm run -n f3 --rootfs /fz /bin/sh /peek", 90)
        assert "# audit:" in out, out                # the header, and
        assert "seq=" not in out, out                # not one record
        assert "id name owner state" in out, out     # the header, and
        assert not re.search(r"(?m)^\d+ \S+ \d+ (running|paused)", out), out

        # the calls that take a process number, given numbers no process
        # can have. A negative one indexed in front of the pid table and
        # the pointer found there was followed: getsid(-1) panicked the
        # machine from any process at all, container or not.
        out = run_cmd(q, "fuzzsys badpid", 60)
        assert "badpid ok" in out, out
        assert "?" not in out, out
        out = run_cmd(q, "vmm run -n fb --rootfs /fz /bin/fuzzsys badpid", 90)
        assert "badpid ok (1)" in out, out    # and it is init in there

        # and now the nonsense: a few thousand calls with arguments drawn
        # out of a hat, each in a child of its own, inside a container
        # with eight processes and 64 MB to its name
        # (in a file: the line editor redraws anything over 74 characters)
        run_cmd(q, "echo 'fuzzsys fuzz 1200 20260924' > /fz/nonsense")
        out = run_cmd(q, "vmm run -n f4 --rootfs /fz --pids 8 --mem 16000 "
                         "/bin/sh /nonsense", 300)
        m = re.search(r"(\d+) calls, (\d+) refused, (\d+) signalled", out)
        assert m, out
        assert int(m.group(1)) > 900, out            # most rounds ran
        assert int(m.group(2)) > 0, out              # and the filter bit
        assert "alive" in out, out
        assert "panic" not in out, out

        # the machine is still itself: its name, its file system, its
        # shell, and it can still start a container
        out = run_cmd(q, "hostname")
        assert "xv6h" in out and "boxy" not in out, out
        out = run_cmd(q, "head -1 /etc/passwd")
        assert "root:x:0:0" in out, out
        out = run_cmd(q, "vmm ls", 30)
        assert "no vms" in out, out
        out = run_cmd(q, "vmm run -n f5 --rootfs /fz /bin/fuzzsys check", 120)
        assert "check ok" in out, out

        # the same again on the machine itself, where the profile is one a
        # program asked for rather than one a runtime imposed: the calls
        # that could end the machine are already given up before the
        # guessing starts, and the machine has to survive the rest.
        out = run_cmd(q, "fuzzsys fuzz 1200 7", 300)
        assert "alive" in out, out
        assert "panic" not in out, out
        out = run_cmd(q, "echo still-here && ls / | grep -c bin")
        assert "still-here" in out, out
        print("    a container's calls are bounded, and nonsense is survived")
    finally:
        q.close()


def t_cgroups(log):
    """control groups: a name, a place in a tree, and limits with teeth.

    The processor's share is the scheduler suite's business (sched-fair
    and sched-group-weight); what is checked here is the tree, the two
    limits that stop a group taking the machine, and freezing."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # the tree: a group is made by name and has a file per control
        run_cmd(q, "echo g1 > /sys/fs/cgroup/cgroup.create", 20)
        out = run_cmd(q, "ls /sys/fs/cgroup/g1")
        for f in ("cpu.weight", "cpu.max", "memory.max", "memory.current",
                  "pids.max", "pids.current", "freeze", "procs", "stat"):
            assert f in out, (f, out)
        run_cmd(q, "echo 5 > /sys/fs/cgroup/g1/pids.max", 20)
        out = run_cmd(q, "cat /sys/fs/cgroup/g1/pids.max")
        assert re.search(r"(?m)^5\s*$", out), out

        # how many processes a group may have. Nine children and the
        # program that made them is ten, and the eleventh is refused
        # exactly as the machine's own limit would refuse it.
        out = run_cmd(q, "cgtest fork 40", 60)
        assert "forks 40 of 40" in out, out
        out = run_cmd(q, "cgrun -n pl --pids 10 cgtest fork 40", 60)
        assert "forks 9 of 40" in out, out
        assert "temporarily unavailable" in out, out
        out = run_cmd(q, "cat /sys/fs/cgroup/pl/stat")
        assert re.search(r"pids.refused [1-9]", out), out

        # how much memory. The machine has plenty; the group does not,
        # and a process in it that asks for more is stopped where it
        # stands -- which is what an out-of-memory kill is.
        out = run_cmd(q, "cgtest mem 800", 90)
        assert "pages 800 of 800" in out, out
        out = run_cmd(q, "cgrun -n ml --mem 300 cgtest mem 800", 90)
        assert "pages 800 of 800" not in out, out
        m = re.findall(r"at (\d+)", out)
        assert m and 100 <= int(m[-1]) <= 400, out
        out = run_cmd(q, "cat /sys/fs/cgroup/ml/stat")
        assert re.search(r"memory.refused [1-9]", out), out
        out = run_cmd(q, "head -2 /proc/meminfo")
        m = re.search(r"MemFree:\s+(\d+)", out)
        assert m and int(m.group(1)) > 500000, out   # the machine is not full

        # freezing: a group that is frozen does nothing, and does it
        # again when it is let go
        run_cmd(q, "rm -f /tmp/cnt")
        q.send("cgrun -n fz --pids 4 cgtest count /tmp/cnt &\n")
        q.expect(r"\$ ", 20)
        time.sleep(2)
        run_cmd(q, "echo 1 > /sys/fs/cgroup/fz/freeze", 20)
        time.sleep(1)
        a = run_cmd(q, "wc -c < /tmp/cnt")
        time.sleep(3)
        b = run_cmd(q, "wc -c < /tmp/cnt")
        na = int(re.search(r"(\d+)", a).group(1))
        nb = int(re.search(r"(\d+)", b).group(1))
        assert nb == na, (a, b)
        out = run_cmd(q, "cat /sys/fs/cgroup/fz/freeze")
        assert re.search(r"(?m)^1\s*$", out), out
        run_cmd(q, "echo 0 > /sys/fs/cgroup/fz/freeze", 20)
        # and it starts again. How soon depends on how busy the machine
        # is, so this waits for it rather than assuming a number of
        # seconds: what is being tested is that it moves at all.
        nc = nb
        for _ in range(20):
            time.sleep(1)
            c = run_cmd(q, "wc -c < /tmp/cnt")
            nc = int(re.search(r"(\d+)", c).group(1))
            if nc > nb:
                break
        assert nc > nb, (b, nc)
        run_cmd(q, "pkill cgtest", 20)

        # a group with processes in it is not taken away, and an empty
        # one is
        out = run_cmd(q, "cgrun --rm fz", 20)
        assert "busy" in out.lower() or out.strip().endswith("$"), out
        run_cmd(q, "cgrun --rm g1", 20)
        out = run_cmd(q, "cgrun --cgls", 20)
        assert "g1" not in out, out

        # and only the machine's root may make one or set a limit
        out = run_cmd(q, "su user -c 'echo x > /sys/fs/cgroup/cgroup.create'", 30)
        assert "permitted" in out.lower() or "denied" in out.lower(), out
        out = run_cmd(q, "su user -c 'echo 1 > /sys/fs/cgroup/pl/pids.max'", 30)
        assert "permitted" in out.lower() or "denied" in out.lower(), out
        print("    a group's limits are what its processes may take")
    finally:
        q.close()


def t_userns(log):
    """user namespaces: root inside is an ordinary user outside, and what
    root inside may do is bounded by its own namespaces."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # the whole of it from inside: the map, becoming root, what that
        # root may and may not do, and who owns what
        out = run_cmd(q, "unstest", 60)
        assert "unstest: all ok" in out, out
        assert "FAILED" not in out, out

        # an ordinary user may make one -- that is the point of them --
        # and is root inside it
        out = run_cmd(q, "su user -c 'unshare -U id -u'", 30)
        assert "65534" in out, out       # nobody, until a map is written
        # and the machine is unmoved by any of it
        out = run_cmd(q, "id -u")
        assert re.search(r"(?m)^0\s*$", out), out
        out = run_cmd(q, "head -1 /etc/passwd")
        assert "root:x:0:0" in out, out

        # a namespace's root may name its own namespace and not the
        # machine: in a user namespace but the machine's uts namespace,
        # setting the hostname is refused
        out = run_cmd(q, "su user -c 'unshare -U hostname nope'", 30)
        assert "not permitted" in out.lower(), out
        out = run_cmd(q, "hostname")
        assert "xv6h" in out and "nope" not in out, out
        print("    root inside is an ordinary user outside")
    finally:
        q.close()


def t_mntns(log):
    """mount namespaces: what a process sees mounted is its own, and a
    process that has pivoted into a new root cannot reach the machine's
    file system by any path.

    (The commands are kept short and the multi-step ones put in files:
    the harness needs every line to fit in the console's line editor.)"""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "mkdir -p /m1 /m2")

        # a mount made in a namespace of its own is not a mount on the
        # machine
        run_cmd(q, "echo 'mount -t ramfs none /m1' > /s1")
        run_cmd(q, "echo 'mount | grep -c /m1' >> /s1")
        out = run_cmd(q, "unshare -m sh /s1", 30)
        assert re.search(r"(?m)^1\s*$", out), out
        out = run_cmd(q, "mount | grep -c /m1")
        assert re.search(r"(?m)^0\s*$", out), out

        # and the machine's mounts are in a copy made after them
        run_cmd(q, "mount -t ramfs none /m2", 30)
        out = run_cmd(q, "unshare -m sh -c 'mount|grep -c /m2'", 30)
        assert re.search(r"(?m)^1\s*$", out), out
        # unmounting inside is inside only
        run_cmd(q, "echo 'umount /m2' > /s2")
        run_cmd(q, "echo 'mount | grep -c /m2' >> /s2")
        out = run_cmd(q, "unshare -m sh /s2", 30)
        assert re.search(r"(?m)^0\s*$", out), out
        out = run_cmd(q, "mount | grep -c /m2")
        assert re.search(r"(?m)^1\s*$", out), out
        run_cmd(q, "umount /m2", 30)

        # the whole of it from inside: a namespace, a mount, a pivot, and
        # what can be reached afterwards
        out = run_cmd(q, "nstest", 60)
        assert "nstest: all ok" in out, out
        assert "FAILED" not in out, out

        # and the machine is untouched by all of it
        out = run_cmd(q, "head -1 /etc/passwd")
        assert "root:x:0:0" in out, out
        out = run_cmd(q, "mount | grep -c newroot")
        assert re.search(r"(?m)^0\s*$", out), out

        # a namespace of one's own is privileged
        out = run_cmd(q, "su user -c 'unshare -m true'", 30)
        assert "not permitted" in out.lower(), out
        print("    what is mounted is a namespace's, and a pivot is a wall")
    finally:
        q.close()


def t_pidns(log):
    """pid namespaces: a process's number is a fact about a namespace, and
    inside a container the first process is number 1."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # the first process in a new namespace is its init
        out = run_cmd(q, "unshare -p sh -c 'echo inside=$$'", 30)
        assert "inside=1" in out, out
        # and a child of it is numbered from there, not from the machine.
        # (A shell given one command execs it rather than forking, so the
        # child has to be a real one: $! is the number the namespace gave
        # it.)
        out = run_cmd(q, "unshare -p sh -c 'sleep 1 & echo child=$!'", 30)
        m = re.search(r"child=(\d+)", out)
        assert m and 1 < int(m.group(1)) < 10, out
        # and the number it was given is the number its parent can wait
        # for. Only the first process of a namespace was being numbered
        # in it: fork handed back the machine's number, wait was told
        # there was no such child, and nothing inside could reap its own.
        out = run_cmd(q, "unshare -p sh -c 'sleep 1 & wait $!; echo rc=$?'", 30)
        assert "rc=0" in out, out
        out = run_cmd(q, "unshare -p sh -c 'sleep 9 & cat /proc/$!/status|head -1'",
                      30)
        assert "sleep" in out, out

        # the machine's own numbering is unchanged, and is large by now
        out = run_cmd(q, "echo outside=$$")
        m = re.search(r"outside=(\d+)", out)
        assert m and int(m.group(1)) > 1, out

        # /proc inside the namespace is the namespace's: its own process
        # is there under its own number, and the machine's init is not
        out = run_cmd(q, "unshare -p sh -c 'head -1 /proc/1/status'", 30)
        assert "sh" in out, out
        out = run_cmd(q, "unshare -p sh -c 'ls /proc | tr \"\\n\" \" \"'", 30)
        nums = [int(w) for w in out.split() if w.isdigit()]
        assert nums and max(nums) < 100, out    # its own few, not the machine's
        out = run_cmd(q, "ls /proc | tr '\\n' ' '")
        outer = [int(w) for w in out.split() if w.isdigit()]
        assert max(outer) > max(nums), (nums, outer)

        # what is outside cannot be signalled from inside, even by the
        # number that would name it outside
        out = run_cmd(q, "unshare -p sh -c 'kill -TERM 2 2>&1; echo rc=$?'", 30)
        assert "rc=0" not in out or "No such process" in out, out
        # and the machine's init is still there
        out = run_cmd(q, "head -1 /proc/1/status")
        assert "init" in out, out

        # a namespace goes when its init does, and its numbers come back:
        # ten in a row, each starting again at 1
        for _ in range(10):
            out = run_cmd(q, "unshare -p sh -c 'echo n=$$'", 30)
            assert "n=1" in out, out

        # the numbers a namespace hands out are its own, so two
        # namespaces at once both start at 1
        out = run_cmd(q, "unshare -p sh -c 'echo a=$$' & unshare -p sh -c"
                         " 'echo b=$$'; wait", 40)
        assert "a=1" in out and "b=1" in out, out

        # and a process behind a namespace is still the machine's to see
        out = run_cmd(q, "unshare -p sleep 20 &", 20)
        m = re.search(r"\[\d+\]\s+(\d+)", out)
        assert m, out
        hostpid = int(m.group(1))
        assert hostpid > 1, out
        out = run_cmd(q, "ps -ef | grep sleep")
        assert "sleep" in out, out
        run_cmd(q, "kill %d" % hostpid, 20)
        print("    a number is a fact about a namespace")
    finally:
        q.close()


def t_shutdown(log):
    """poweroff and reboot: the services stopped, what is waiting
    written, and the machine actually stopping."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "echo written-before-poweroff > /pmark")
        q.send("poweroff\n")
        q.expect(r"reboot: power off", 30)
        # the machine stops rather than coming back to a prompt
        for _ in range(30):
            if q.proc.poll() is not None:
                break
            time.sleep(0.2)
        assert q.proc.poll() is not None, "qemu did not exit"
    finally:
        q.close()

    # and what was written before it is there afterwards: the same
    # disk, not a fresh copy of it
    q = QEMU(intc="plic", cpus=3, log=log, keep_fs=True)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        out = run_cmd(q, "cat /pmark")
        assert "written-before-poweroff" in out, out
        run_cmd(q, "rm /pmark")
        run_cmd(q, "sync", 30)
        print("    poweroff stops the machine, and what was written is still there")
    finally:
        q.close()

    # and the line in /etc/inittab that says what ctrl-alt-del does:
    #
    #   ca::ctrlaltdel:/sbin/shutdown -r now
    #
    # init takes SIGINT to mean that key, as every init does, so an
    # interrupt sent to process 1 reboots the machine. It is worth a test
    # because it surprises people -- a stray ^C aimed at a program, or a
    # test harness sending one down the console, reboots rather than
    # interrupting -- and because the surprise is the configuration
    # working, not failing.
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        out = run_cmd(q, "grep ctrlaltdel /etc/inittab")
        assert "shutdown -r now" in out, out
        q.send("kill -INT 1\n")
        # what a reboot looks like: the services stopped, then a machine
        # coming up again from the beginning
        q.expect(r"(?i)(restart|reboot|Stopping)", 30)
        q.expect(r"xv6 kernel is booting", 60)
        q.expect(r"init: starting sh", 90)
        q.expect(r"\$ ", 20)
        out = run_cmd(q, "uptime")
        assert "up" in out, out
        print("    an interrupt to init is ctrl-alt-del, and reboots the machine")
    finally:
        q.close()


def t_manual(log):
    """every utility has a manual page, and answers --help from it.

    The check is the list of what is installed against the list of what
    is documented, made on the machine itself: a program added without a
    page fails here, which is the only way a manual of two hundred pages
    stays true."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # what is installed, and what is documented
        out = run_cmd(q, "ls /bin /sbin | sort -u > /tmp/have", 30)
        out = run_cmd(q, "man | cut -d' ' -f1 | sort -u > /tmp/doc", 30)
        out = run_cmd(q, "comm -23 /tmp/have /tmp/doc", 30)
        undocumented = [l for l in out.splitlines()
                        if l.strip() and not l.startswith("$") and
                        "comm -23" not in l and ":" not in l]
        assert not undocumented, "no manual page for: %s" % undocumented

        # a page is a page, and --help is the same words
        out = run_cmd(q, "man ls", 20)
        assert "NAME" in out and "SYNOPSIS" in out and "list directory" in out, out
        out = run_cmd(q, "ls --help", 20)
        assert "SYNOPSIS" in out and "DESCRIPTION" not in out, out
        out = run_cmd(q, "vmm --help", 20)
        assert "vmm migrate" in out, out
        out = run_cmd(q, "man -k namespace | wc -l", 20)
        assert re.search(r"(?m)^[2-9]\d*\s*$", out), out
        out = run_cmd(q, "man nosuchthing", 20)
        assert "no manual entry" in out, out

        # and a page the reader can trust: the synopsis names the
        # program, not another one
        for name in ("cp", "grep", "mount", "unshare", "tar", "dd"):
            out = run_cmd(q, "man %s | head -5" % name, 20)
            assert re.search(r"(?m)^\s+%s - " % name, out), out
            assert "SYNOPSIS" in out, out
        print("    every installed utility has a page, and --help reads it")
    finally:
        q.close()


def t_utils(log):
    """the file and system utilities: each one asked something it has
    to get right, and the pipeline they make together."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        out = run_cmd(q, "uname -s -m")
        assert "xv6H" in out and "riscv64" in out, out
        out = run_cmd(q, "nproc")
        assert re.search(r"(?m)^3\s*$", out), out
        out = run_cmd(q, "hostname")
        assert "xv6h" in out, out

        # numbers, and the separator attached to the option
        out = run_cmd(q, "seq 1 5")
        assert re.search(r"(?m)^1\n2\n3\n4\n5", out.replace("\r", "")), out
        out = run_cmd(q, "seq -s, 1 5")
        assert "1,2,3,4,5" in out, out

        # lines in and out of order
        out = run_cmd(q, "printf 'b\\na\\nc\\na\\n' | sort -u | tr -d '\\n'")
        assert "abc" in out, out
        out = run_cmd(q, "printf 'a\\na\\nb\\n' | uniq -c | tr -s ' '")
        assert "2 a" in out and "1 b" in out, out
        out = run_cmd(q, "printf 'a:1\\nb:2\\n' | cut -d: -f2 | tr -d '\\n'")
        assert "12" in out, out
        out = run_cmd(q, "echo hello | tr a-z A-Z")
        assert "HELLO" in out, out

        # the two ends of a file
        out = run_cmd(q, "head -1 /etc/passwd")
        assert len([l for l in out.split("\n") if l.startswith("root:x:0:0")]) == 1, out
        out = run_cmd(q, "tail -1 /etc/passwd")
        assert "nobody" in out, out
        out = run_cmd(q, "wc -l /etc/passwd")
        assert re.search(r"6 /etc/passwd", out), out

        # making and copying
        run_cmd(q, "touch /t1")
        out = run_cmd(q, "ls -l /t1")
        assert "-rw-r--r--" in out, out   # the umask init set
        out = run_cmd(q, "echo hi | tee /t2 && cat /t2")
        # twice: once from tee's own output and once from the file
        assert len([l for l in out.split("\n") if l.strip() == "hi"]) == 2, out
        out = run_cmd(q, "basename /a/b/c.txt .txt")
        assert re.search(r"(?m)^c$", out), out
        out = run_cmd(q, "dirname /a/b/c")
        assert re.search(r"(?m)^/a/b$", out), out

        # what the machine has
        out = run_cmd(q, "df -h")
        assert re.search(r"(?m)^vda\s", out) and "/" in out, out
        out = run_cmd(q, "du -s /etc")
        assert re.search(r"^\d+\s+/etc", out.split("\n")[1].strip()), out
        out = run_cmd(q, "free -m")
        assert "Mem:" in out and "MB" in out, out
        out = run_cmd(q, "uptime")
        assert " up " in out, out
        out = run_cmd(q, "dmesg | wc -l")
        m = re.search(r"(?m)^(\d+)\s*$", out)
        assert m and int(m.group(1)) > 5, out

        # what is running, by name and by number
        out = run_cmd(q, "ps")
        assert re.search(r"(?m)^\s*\d+\s+\d+\s+\S+\s+sh$", out), out
        out = run_cmd(q, "ps -ef")
        assert "root" in out and "init" in out and "syslogd" in out, out
        out = run_cmd(q, "ps -o pid,user,comm")
        assert re.search(r"(?m)^\d+ root sh$", out), out
        out = run_cmd(q, "pgrep -l syslogd")
        assert "syslogd" in out, out

        # the paths a name resolves to
        out = run_cmd(q, "which ls")
        assert "/bin/ls" in out, out
        out = run_cmd(q, "realpath /bin/../etc/passwd")
        assert re.search(r"(?m)^/etc/passwd$", out), out
        out = run_cmd(q, "tty")
        assert "/dev/console" in out, out

        # chroot: a process that cannot climb out of where it was put
        run_cmd(q, "mkdir -p /jail/bin")
        run_cmd(q, "cp /rescue/ls /jail/bin/ls", 30)
        run_cmd(q, "echo inside > /jail/hello")
        out = run_cmd(q, "chroot /jail /bin/ls /", 30)
        listed = [l.strip() for l in out.split("\n")[1:] if l.strip() and l.strip() != "$"]
        assert "hello" in listed and "jail" not in listed, out
        out = run_cmd(q, "chroot /jail /bin/ls /..", 30)
        assert "hello" in out, out      # ".." at the root stays there
        out = run_cmd(q, "chroot /jail /bin/ls /jail", 30)
        assert "No such file" in out, out
        out = run_cmd(q, "su user -c 'chroot /jail /bin/ls /'", 30)
        assert "not permitted" in out, out
        print("    the file and system utilities, ps, and chroot")
    finally:
        q.close()


def t_logging(log):
    """the log: syslogd reading /dev/log, klogd reading /dev/kmsg, and
    the files each message ends up in."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # the kernel's own messages, kept from before anything started
        out = run_cmd(q, "grep booting /var/log/kern.log", 30)
        assert "xv6 kernel is booting" in out, out
        out = run_cmd(q, "ls -l /var/log")
        assert "-rw-r-----" in out, out   # not world-readable

        # a message from a program
        run_cmd(q, "logger -t hello a message", 30)
        out = run_cmd(q, "grep hello /var/log/messages", 30)
        assert "hello" in out and "a message" in out, out

        # the facility decides which file, and auth goes to its own
        run_cmd(q, "logger -p auth.info from-logger", 30)
        out = run_cmd(q, "grep from-logger /var/log/auth.log", 30)
        assert "from-logger" in out, out

        # and what su did is in the log whether or not anyone asked
        run_cmd(q, "su user -c whoami", 30)
        out = run_cmd(q, "grep 'su to' /var/log/auth.log", 30)
        assert "su to user" in out, out

        # /dev/log and /dev/kmsg are devices with the modes they should
        out = run_cmd(q, "ls -l /dev")
        assert re.search(r"crw-rw-rw-.*\blog\s*$", out, re.M), out
        assert re.search(r"crw-r-----.*\bkmsg\s*$", out, re.M), out
        assert re.search(r"brw-rw----.*\bvda\s*$", out, re.M), out
        print("    syslogd, klogd, logger, and the files they write")
    finally:
        q.close()


def t_users(log):
    """users, groups and permission: who a process is, what a file
    allows, and the set-user-id bit that lets su cross between them."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # the accounts the image was built with
        out = run_cmd(q, "id")
        assert "uid=0(root)" in out and "gid=0(root)" in out, out
        assert "root" in run_cmd(q, "whoami")
        out = run_cmd(q, "id user")
        assert "uid=1000(user)" in out, out

        # a file's mode and owner, set and read back
        run_cmd(q, "echo secret > /s")
        run_cmd(q, "chmod 600 /s")
        out = run_cmd(q, "ls -l /s")
        assert re.search(r"^-rw-------\s+1\s+root\s+root\s+7\s+\S+\s+\d+\s+[\d:]+\s+/s$",
                         out.split("\n")[1].strip()), out
        run_cmd(q, "chmod u+x,go-r /s")
        assert "-rwx------" in run_cmd(q, "ls -l /s")
        run_cmd(q, "chown user /s")
        run_cmd(q, "chgrp wheel /s")
        out = run_cmd(q, "ls -l /s")
        assert re.search(r"-rwx------\s+1\s+user\s+wheel\s", out), out
        # -n asks for the numbers instead
        assert re.search(r"-rwx------\s+1\s+1000\s+10\s", run_cmd(q, "ls -ln /s")), out

        # /etc/shadow is root's alone, and su is set-user-id
        out = run_cmd(q, "ls -l /etc/shadow")
        assert "-rw-------" in out, out
        out = run_cmd(q, "ls -l /bin/su")
        assert "-rwsr-xr-x" in out, out

        # su becomes that user, and the checks then bite
        out = run_cmd(q, "su user -c whoami", 30)
        assert "user" in out, out
        out = run_cmd(q, "su user -c id", 30)
        assert "uid=1000(user)" in out, out
        run_cmd(q, "mkdir /pd")
        run_cmd(q, "chmod 700 /pd")
        out = run_cmd(q, "su user -c 'ls /pd'", 30)
        assert "Permission denied" in out, out
        out = run_cmd(q, "su user -c 'rm /s'", 30)
        assert "Permission denied" in out, out
        # ... and what the user may do, it may do
        out = run_cmd(q, "su user -c 'cat /s'", 30)
        assert "secret" in out, out

        # a wrong password is refused, and the right one accepted
        out = run_cmd(q, "su user -c true < /dev/null", 30)
        assert "Authentication" not in out, out  # root is not asked

        # the password file the build wrote is one login can check
        out = run_cmd(q, "passwd -S user", 30)
        assert re.search(r"(?m)^user P$", out), out

        # sudo reads /etc/sudoers: root and %wheel may run anything
        out = run_cmd(q, "sudo -l", 30)
        assert "(ALL) ALL" in out, out
        out = run_cmd(q, "su user -c 'sudo -l'", 30)
        assert "(ALL) ALL" in out, out   # user is in wheel
        out = run_cmd(q, "sudo -u user whoami", 30)
        assert "user" in out, out
        # nobody is in no group that sudoers names
        out = run_cmd(q, "su nobody -c 'sudo -l'", 30)
        assert "not in the sudoers file" in out or "not available" in out, out

        # the account tools, and what they leave in the two files
        run_cmd(q, "useradd -m -G wheel alice", 30)
        out = run_cmd(q, "id alice", 30)
        assert "uid=1001(alice)" in out, out
        out = run_cmd(q, "groups alice", 30)
        assert "wheel" in out, out
        out = run_cmd(q, "ls -ld /home/alice")
        assert "drwxr-xr-x" in out, out      # the mode mkdir asked for
        out = run_cmd(q, "passwd -S alice", 30)
        assert re.search(r"(?m)^alice L$", out), out  # no password yet
        run_cmd(q, "groupadd testers", 30)
        run_cmd(q, "usermod -G testers alice", 30)
        out = run_cmd(q, "groups alice", 30)
        assert "testers" in out and "wheel" not in out, out
        run_cmd(q, "userdel -r alice", 30)
        out = run_cmd(q, "id alice", 30)
        assert "no such user" in out, out
        run_cmd(q, "groupdel testers", 30)
        print("    accounts, modes, owners, set-user-id su, sudo and the account tools")
    finally:
        q.close()


def t_cowimage(log):
    """overlay images: many devices over one base, each with its own
    changes, and the base never written."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "losetup -b /disk0.img /dev/loop0 /d1.cow", 60)
        run_cmd(q, "losetup -b /disk0.img /dev/loop1 /d2.cow", 60)
        out = run_cmd(q, "losetup")
        assert out.count("copy-on-write") == 2, out
        assert "0 of 4096 blocks changed" in out, out

        run_cmd(q, "mkdir /a /b")
        run_cmd(q, "mount -t ext4 /dev/loop0 /a", 30)
        run_cmd(q, "mount -t ext4 /dev/loop1 /b", 30)
        run_cmd(q, "echo from-a > /a/mark")
        run_cmd(q, "echo from-b > /b/mark")
        assert "from-a" in run_cmd(q, "cat /a/mark")
        assert "from-b" in run_cmd(q, "cat /b/mark")
        run_cmd(q, "sync", 60)
        out = run_cmd(q, "losetup")
        assert re.search(r"loop0.*\b(\d+) of 4096 blocks changed", out), out
        assert " 0 of 4096" not in out, out
        run_cmd(q, "umount /a", 60)
        run_cmd(q, "umount /b", 60)
        run_cmd(q, "losetup -d /dev/loop0")
        run_cmd(q, "losetup -d /dev/loop1")

        # the delta is small, and reattaching it brings the changes back
        out = run_cmd(q, "ls -l /d1.cow")
        m = re.search(r"(\d+)\s+\S+\s+\d+\s+[\d:]+\s+/d1.cow", out)
        assert m and int(m.group(1)) < 2 * 1024 * 1024, out
        run_cmd(q, "losetup -b /disk0.img /dev/loop0 /d1.cow", 60)
        run_cmd(q, "mount -t ext4 /dev/loop0 /a", 30)
        assert "from-a" in run_cmd(q, "cat /a/mark")
        run_cmd(q, "umount /a", 60)
        run_cmd(q, "losetup -d /dev/loop0")

        # and the base image itself was never written
        run_cmd(q, "mkdir /base")
        run_cmd(q, "mount -t ext4 -o loop /disk0.img /base", 60)
        out = run_cmd(q, "ls /base")
        assert "mark" not in out.split(), out
        run_cmd(q, "umount /base", 60)
        print("    two devices over one base image, each with its own changes")
    finally:
        q.close()


def t_overlay(log):
    """one tree laid over another: what is read comes from above when
    it is there and below when it is not, what is written always goes
    above, and a name that is removed leaves a whiteout so the one
    below it does not come back."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "mkdir /lo /up /wk /mg")
        run_cmd(q, "echo base > /lo/a")
        run_cmd(q, "echo keep > /lo/b")
        run_cmd(q, "mkdir /lo/d")
        run_cmd(q, "echo deep > /lo/d/c")
        run_cmd(q, "ln -s a /lo/l")
        run_cmd(q, "setfattr -n user.low -v below /lo/a")
        out = run_cmd(q, "mount -t overlay -o lowerdir=/lo,upperdir=/up ovl /mg", 30)
        assert "on /mg type overlay" in out, out

        # everything below shows through
        out = run_cmd(q, "ls /mg")
        for n in ("a", "b", "d", "l"):
            assert re.search(r"(?m)^%s\s*$" % n, out), (n, out)
        assert "base" in run_cmd(q, "cat /mg/a")
        assert "deep" in run_cmd(q, "cat /mg/d/c")
        assert "base" in run_cmd(q, "cat /mg/l")  # the link, followed

        # writing copies the file up and leaves the one below alone
        run_cmd(q, "echo changed > /mg/a")
        assert "changed" in run_cmd(q, "cat /mg/a")
        assert "base" in run_cmd(q, "cat /lo/a")
        assert re.search(r"(?m)^a\s*$", run_cmd(q, "ls /up"))
        # and the copy keeps the attributes the file had
        out = run_cmd(q, "getfattr -d /mg/a")
        assert 'user.low="below"' in out, out

        # a new name goes above
        run_cmd(q, "echo new > /mg/n")
        assert re.search(r"(?m)^n\s*$", run_cmd(q, "ls /up"))

        # a removed name leaves a whiteout, and stays removed
        run_cmd(q, "rm /mg/b")
        out = run_cmd(q, "ls /mg")
        assert not re.search(r"(?m)^b\s*$", out), out
        assert "cannot open" in run_cmd(q, "cat /mg/b")
        assert re.search(r"(?m)^b\s*$", run_cmd(q, "ls /lo"))
        out = run_cmd(q, "ls -l /up")
        assert re.search(r"(?m)^c[rwx-]{9}.*\bb\s*$", out), out  # the whiteout is a device node

        # a file deep below brings its directory up with it
        run_cmd(q, "echo x >> /mg/d/c")
        out = run_cmd(q, "cat /mg/d/c")
        assert "deep" in out and "x" in out, out
        assert "x" not in run_cmd(q, "cat /lo/d/c")
        assert re.search(r"(?m)^c\s*$", run_cmd(q, "ls /up/d"))

        # and a directory made in the overlay works like any other
        run_cmd(q, "mkdir /mg/newdir")
        run_cmd(q, "echo z > /mg/newdir/f")
        assert "z" in run_cmd(q, "cat /mg/newdir/f")

        run_cmd(q, "umount /mg", 30)
        out = run_cmd(q, "ls /mg")
        assert not re.search(r"(?m)^a\s*$", out), out
        print("    overlay: merged reads, copy-up on write, whiteouts")
    finally:
        q.close()


def t_mount_privilege(log):
    """mount and umount are the machine's to do, not everybody's.

    Nothing checked before this: any user could mount a file system over
    any directory they could write -- a ramfs over /etc -- or unmount
    what somebody else had mounted."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "mkdir -p /tmp/mp")

        # root may
        out = run_cmd(q, "mount -t ramfs none /tmp/mp && echo mounted", 30)
        assert "mounted" in out, out
        out = run_cmd(q, "mount | grep /tmp/mp")
        assert "ramfs" in out, out

        # and an ordinary user may not unmount it
        out = run_cmd(q, "su user -c 'umount /tmp/mp'", 30)
        assert "not permitted" in out.lower(), out
        out = run_cmd(q, "mount | grep /tmp/mp")
        assert "ramfs" in out, out
        run_cmd(q, "umount /tmp/mp", 30)

        # nor mount anything anywhere
        out = run_cmd(q, "su user -c 'mount -t ramfs none /tmp/mp'", 30)
        assert "not permitted" in out.lower(), out
        out = run_cmd(q, "mount | grep /tmp/mp")
        assert "ramfs" not in out, out
        # not even over a directory of their own
        run_cmd(q, "su user -c 'mkdir -p /tmp/own'", 20)
        out = run_cmd(q, "su user -c 'mount -t tmpfs none /tmp/own'", 30)
        assert "not permitted" in out.lower(), out
        # and the refusal is recorded, like every other privileged thing
        out = run_cmd(q, "grep -c 'umount' /proc/audit", 20)
        assert re.search(r"(?m)^[1-9]", out), out
        print("    mounting is privileged, and refusals are recorded")
    finally:
        q.close()


def t_mountopts(log):
    """the mount options: read-only and remount, bind, noexec, nodev,
    and -o loop attaching a loop device by itself."""
    img = os.path.join(HERE, "test-mo.img")
    subprocess.run([os.path.join(HERE, "mkfs", "mkext4"), "-b", "4096", "-j", "1024",
                    "-L", "opts", img], check=True, stdout=subprocess.DEVNULL)
    q = QEMU(intc="plic", cpus=3, log=log, extra_env={"EXTRADISK": img})
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "mkdir /m")

        # read-only, and the remount that makes it writable
        out = run_cmd(q, "mount -t ext4 -o ro /dev/vdb /m", 30)
        assert "(ro)" in out, out
        out = run_cmd(q, "echo x > /m/f")
        assert "$" in out and "x" not in out.split("\n")[-1], out
        out = run_cmd(q, "mount -o remount,rw /dev/vdb /m", 30)
        assert "remounted rw" in out, out
        run_cmd(q, "echo x > /m/f")
        out = run_cmd(q, "cat /m/f")
        assert "x" in out, out
        out = run_cmd(q, "mount -o remount,ro /dev/vdb /m", 30)
        assert "remounted ro" in out, out
        out = run_cmd(q, "rm /m/f")
        assert "Read-only" in out, out
        run_cmd(q, "mount -o remount,rw /dev/vdb /m", 30)

        # a bind mount is a second name for a directory: the same
        # inodes, reached by another path
        run_cmd(q, "mkdir /m/sub")
        run_cmd(q, "echo deep > /m/sub/g")
        run_cmd(q, "mkdir /b")
        out = run_cmd(q, "mount --bind /m/sub /b", 30)
        assert "type bind" in out, out
        out = run_cmd(q, "cat /b/g")
        assert "deep" in out, out
        run_cmd(q, "echo more > /b/h")
        out = run_cmd(q, "cat /m/sub/h")
        assert "more" in out, out
        run_cmd(q, "umount /b", 30)
        out = run_cmd(q, "ls /b")
        assert "h" not in out.split(), out

        # noexec: the file can be read and copied, and not run
        run_cmd(q, "mkdir /n")
        run_cmd(q, "mount -t tmpfs -o noexec none /n", 30)
        run_cmd(q, "cp /bin/echo /n/echo", 60)
        out = run_cmd(q, "/n/echo hi")
        assert "Permission denied" in out, out
        run_cmd(q, "umount /n", 30)

        # -o loop: the mount attaches the loop device and detaches it
        out = run_cmd(q, "dd if=/dev/vdb of=/i.img bs=65536", 300)
        assert "bytes copied" in out, out
        run_cmd(q, "umount /m", 60)
        out = run_cmd(q, "mount -t ext4 -o loop /i.img /m", 60)
        assert "on /m type ext4" in out, out
        out = run_cmd(q, "cat /m/sub/g")
        assert "deep" in out, out
        out = run_cmd(q, "losetup")
        assert "/i.img" in out, out
        run_cmd(q, "umount /m", 60)
        out = run_cmd(q, "losetup")
        assert "/i.img" not in out, out
        print("    ro, remount, bind, noexec and -o loop")
    finally:
        q.close()
        try:
            os.unlink(img)
        except OSError:
            pass


def t_sysfs(log):
    """/sys: the kernel's objects as a tree of one-line files, built
    from what the kernel has rather than from a list, so a disk that
    appears is in it."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        out = run_cmd(q, "ls /sys")
        for d in ("kernel", "block", "fs", "hypervisor", "vector"):
            assert re.search(r"(?m)^%s\s*$" % d, out), (d, out)

        out = run_cmd(q, "cat /sys/kernel/ostype")
        assert "xv6H" in out, out
        out = run_cmd(q, "cat /sys/kernel/ncpu")
        assert re.search(r"(?m)^3\s*$", out), out
        out = run_cmd(q, "cat /sys/kernel/pagesize")
        assert re.search(r"(?m)^4096\s*$", out), out

        # a disk is there with its size in sectors, and so is the
        # queue's directory under it
        out = run_cmd(q, "ls /sys/block")
        assert re.search(r"(?m)^vda\s*$", out), out
        out = run_cmd(q, "cat /sys/block/vda/size")
        m = re.search(r"(?m)^(\d+)\s*$", out)
        assert m and int(m.group(1)) > 1000, out
        out = run_cmd(q, "cat /sys/block/vda/queue/hw_sector_size")
        assert re.search(r"(?m)^512\s*$", out), out

        # every mount has a directory, and the root says it is ext4
        out = run_cmd(q, "cat /sys/fs/root/type")
        assert "ext4" in out, out
        out = run_cmd(q, "cat /sys/fs/sys/type")
        assert "sysfs" in out, out

        # a loop device that appears is in /sys without anyone saying so
        run_cmd(q, "dd if=/dev/zero of=/l.img bs=65536 count=16", 60)
        run_cmd(q, "losetup /dev/loop0 /l.img")
        out = run_cmd(q, "cat /sys/block/loop0/size")
        assert re.search(r"(?m)^2048\s*$", out), out
        run_cmd(q, "losetup -d /dev/loop0")

        # what the machine has
        # the same for the hypervisor extension: `--axis hyp=0' hides it,
        # and /sys then says 0, which is the machine it is on being
        # described correctly.
        out = run_cmd(q, "cat /sys/hypervisor/present")
        assert re.search(r"(?m)^[01]\s*$", out), out
        # the vector unit as this machine has it: /sys says what is there,
        # and on a machine built without the V extension (`--axis vec=0')
        # what is there is nothing, which is the right answer and not a
        # failure.
        out = run_cmd(q, "cat /sys/vector/present")
        has_v = re.search(r"(?m)^1\s*$", out) is not None
        out = run_cmd(q, "cat /sys/vector/vlen")
        m = re.search(r"(?m)^(\d+)\s*$", out)
        assert m, out
        vlen = int(m.group(1))
        if not has_v:
            assert vlen == 0, out
        else:
            # whatever width this machine was built with (`--axis
            # vlen=512'), and the same number the kernel printed at boot:
            # what is being tested is that /sys agrees with the machine,
            # not that the machine is 128 bits wide.
            assert vlen >= 128 and (vlen & (vlen - 1)) == 0, out
            boot = q.buf.decode(errors="replace")
            bm = re.search(r"vector extension: vlen (\d+) bits", boot)
            assert bm and int(bm.group(1)) == vlen, (vlen, bm and bm.group(1))

        # and /tmp is a tmpfs init mounted
        out = run_cmd(q, "cat /sys/fs/tmp/type")
        assert "tmpfs" in out, out
        run_cmd(q, "echo t > /tmp/x")
        out = run_cmd(q, "cat /tmp/x")
        assert "t" in out, out
        print("    /sys: the kernel, the disks, the mounts and the machine")
    finally:
        q.close()


def t_aslr(log):
    """address space layout randomisation: the same program run twice is
    given different addresses, and with the knob off it is given the
    same ones. What moves at each level is checked separately, because
    "some of the numbers changed" would pass even if only the stack
    were moving."""
    # 512 MB rather than the default gigabyte, and the reason is `execout'
    # below. It allocates every page the machine has, one page at a time,
    # touching each, and does that fifteen times over -- so its cost is
    # proportional to RAM and has nothing to do with what this test is about.
    # Measured on an idle 32-core box with a gigabyte: **214 seconds**, against
    # the 300-second allowance the loop below gives it. That is no headroom at
    # all, which is why this test failed on a busy laptop, failed twelve times
    # out of twelve when run in parallel, and looked like a regression at three
    # different commits. Half the memory is half the work and the coverage is
    # identical: execout still exhausts the machine and still execs with nothing
    # left, just with less to exhaust. (It skips itself only above 4 GB, so
    # going down does not lose the test.)
    q = QEMU(intc="plic", cpus=3, mem="512M", log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        def addrs():
            out = run_cmd(q, "aslrcheck", 30)
            m = re.search(r"text=0x([0-9a-f]+) libc=0x([0-9a-f]+) "
                          r"stack=0x([0-9a-f]+) heap=0x([0-9a-f]+) "
                          r"mmap=0x([0-9a-f]+) brk=0x([0-9a-f]+)", out)
            assert m, out
            keys = ("text", "libc", "stack", "heap", "mmap", "brk")
            return dict(zip(keys, (int(g, 16) for g in m.groups())))

        # the default, 2: everything moves. Ten runs, and every one of
        # the five addresses has to take at least two different values
        # -- with a 256-page stack slide a pair could collide by chance,
        # but not ten runs' worth.
        out = run_cmd(q, "cat /sys/kernel/randomize_va_space")
        assert re.search(r"(?m)^2\s*$", out), out
        runs = [addrs() for _ in range(10)]
        for k in runs[0]:
            seen = set(r[k] for r in runs)
            assert len(seen) > 1, (k, [hex(v) for v in seen])
        print("    2: text, libc, stack, heap, mmap and brk all move")

        # 0: nothing moves. Every address is identical across runs.
        run_cmd(q, "echo 0 > /sys/kernel/randomize_va_space")
        out = run_cmd(q, "cat /sys/kernel/randomize_va_space")
        assert re.search(r"(?m)^0\s*$", out), out
        a, b = addrs(), addrs()
        assert a == b, (a, b)
        print("    0: two runs report identical addresses")

        # 1: the stack, the mmap area and the shared library move; the
        # program's own text and its heap, which grows from the end of
        # that text, do not.
        run_cmd(q, "echo 1 > /sys/kernel/randomize_va_space")
        runs = [addrs() for _ in range(8)]
        # (malloc in this libc maps its arena rather than growing the
        # heap, so the block it returns moves with the mmap area; the
        # kernel's heap, which brk reports, is what stays put at 1.)
        for k in ("stack", "libc", "mmap", "heap"):
            assert len(set(r[k] for r in runs)) > 1, (k, runs)
        for k in ("text", "brk"):
            assert len(set(r[k] for r in runs)) == 1, (k, runs)
        print("    1: stack, libc and mmap move; text and the heap do not")

        # a bad value is refused and leaves the setting alone
        run_cmd(q, "echo 3 > /sys/kernel/randomize_va_space")
        out = run_cmd(q, "cat /sys/kernel/randomize_va_space")
        assert re.search(r"(?m)^1\s*$", out), out

        run_cmd(q, "echo 2 > /sys/kernel/randomize_va_space")
        # and the randomised stack is still a working stack: a program
        # that recurses and a program that maps still run.
        # three hundred seconds rather than a hundred and eighty: these
        # three take about a hundred together on a quiet laptop and twice
        # that on a warm one, and what is being tested is that a
        # randomised stack works, not how fast.
        for t in ("bigargtest", "stacktest", "execout"):
            out = run_cmd(q, "usertests " + t, 300)
            assert "ALL TESTS PASSED" in out, (t, out)
        print("    a randomised stack is still a stack: "
              "bigargtest, stacktest, execout")
    finally:
        q.close()


def t_audit(log):
    """the audit log: the privileged things that happen are written down,
    a program can add a record of its own through /dev/audit, and
    somebody who is not root can see that there is a log but not what
    is in it."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)

        # booting mounts five file systems, and each one is a record
        out = run_cmd(q, "cat /proc/audit")
        m = re.search(r"# audit: (\d+) records, (\d+) dropped", out)
        assert m and int(m.group(1)) > 0, out
        assert re.search(r"op=mount what=/dev .* res=ok", out), out
        assert re.search(r"op=mount what=/proc .* res=ok", out), out
        before = int(m.group(1))

        # su becomes another user, and that is written down
        run_cmd(q, "su user -c id", 30)
        out = run_cmd(q, "cat /proc/audit")
        assert re.search(r"comm=su op=setuid .* b=1000 res=ok", out), out

        # a mount and an unmount of this run's own
        run_cmd(q, "mkdir -p /a")
        run_cmd(q, "mount -t tmpfs none /a", 30)
        run_cmd(q, "umount /a", 30)
        out = run_cmd(q, "cat /proc/audit")
        assert re.search(r"op=mount what=/a .* res=ok", out), out
        assert re.search(r"op=umount what=/a .* res=ok", out), out

        # a program's own record
        run_cmd(q, "echo the-suite-was-here > /dev/audit")
        out = run_cmd(q, "cat /proc/audit")
        assert "op=user msg=the-suite-was-here" in out, out

        # and it has grown
        m = re.search(r"# audit: (\d+) records", out)
        assert m and int(m.group(1)) > before, (before, out)

        # somebody who is not root sees that there is a log and nothing
        # else, and cannot add to it either
        out = run_cmd(q, "su user -c 'cat /proc/audit'", 30)
        assert "# audit:" in out, out
        assert "op=mount" not in out and "op=setuid" not in out, out
        out = run_cmd(q, "su user -c 'echo sneaky > /dev/audit'", 30)
        assert "Permission denied" in out, out
        out = run_cmd(q, "cat /proc/audit")
        assert "sneaky" not in out, out
        print("    mounts, setuid, a program's own record, and not for everybody")
    finally:
        q.close()


def t_ramfs(log):
    """a file system that is nothing but memory: tmpfs with a limit and
    ramfs without one, both mounted, written, filled and unmounted."""
    q = QEMU(intc="plic", cpus=3, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        run_cmd(q, "mkdir /t")
        out = run_cmd(q, "mount -t tmpfs none /t", 30)
        assert "on /t type tmpfs" in out, out

        run_cmd(q, "echo hello > /t/a")
        out = run_cmd(q, "cat /t/a")
        assert "hello" in out, out
        run_cmd(q, "mkdir /t/d")
        run_cmd(q, "echo deep > /t/d/b")
        out = run_cmd(q, "cat /t/d/b")
        assert "deep" in out, out
        run_cmd(q, "ln -s a /t/l")
        out = run_cmd(q, "cat /t/l")
        assert "hello" in out, out
        out = run_cmd(q, "ls -l /t")
        assert re.search(r"l[rwx-]{9}.*\bl -> a", out), out

        # attributes live in memory too, with no format to refuse a name
        run_cmd(q, "setfattr -n user.x -v inmem /t/a")
        out = run_cmd(q, "getfattr -d /t/a")
        assert 'user.x="inmem"' in out, out

        # a file larger than one page, through the two-level page table
        out = run_cmd(q, "dd if=/dev/zero of=/t/big bs=65536 count=64", 120)
        assert "4194304 bytes copied" in out, out
        out = run_cmd(q, "ls -l /t/big")
        assert "4194304" in out, out

        # a directory goes only when it is empty
        out = run_cmd(q, "rmdir /t/d")
        assert "not empty" in out.lower() or "Directory not empty" in out, out
        run_cmd(q, "rm /t/d/b")
        run_cmd(q, "rmdir /t/d")
        out = run_cmd(q, "ls /t")
        assert "d" not in out.split(), out

        # the limit is real: a tmpfs of 1 MB does not take 4
        run_cmd(q, "mkdir /t2")
        run_cmd(q, "mount -t tmpfs -o size=1M none /t2", 30)
        out = run_cmd(q, "dd if=/dev/zero of=/t2/fill bs=65536 count=64", 120)
        assert "No space left" in out, out
        run_cmd(q, "umount /t2", 30)

        # everything goes when the mount does
        run_cmd(q, "umount /t", 30)
        out = run_cmd(q, "ls /t")
        assert out.strip().splitlines()[-1].strip() in ("$", ""), out
        print("    tmpfs: files, directories, links, attributes and a size limit")
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
            cmd += ["HOSTFS_MB=192", "HOSTFILES=linux/Image linux/linux.img"]
        subprocess.run(cmd, cwd=HERE, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


TIMELINE = re.compile(rb"hyp: vm \d+: ((?:crossing, this side|arrival timeline): [^\r\n]*)")

def timeline_report():
    """L15: the crossing's timeline as the two hosts printed it -- the source's
    "crossing, this side: stopped +0, last page +A us, ..." and the destination's
    "arrival timeline: ..." -- kept with the test's own output, because a queue's
    logs keep no consoles (tools/timeline_table.py reads them from either)."""
    for i, q in enumerate(QEMU.live):
        for m in TIMELINE.finditer(q.buf):
            print("    host %d: %s" % (i, m.group(1).decode(errors="replace")))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("suite", nargs="?", default="all")
    ap.add_argument("-i", "--intc", default=None, help="plic|aplic|imsic (default: all)")
    ap.add_argument("-c", "--cpus", type=int, default=3)
    ap.add_argument("-v", action="store_true", help="tee qemu output to stdout")
    ap.add_argument("-t", "--test", default=None, help="only tests whose name matches this (a pattern)")
    ap.add_argument("--append", default="", metavar="WORDS",
                    help="host command-line words for every emulated host (e.g. admeasure=1)")
    ap.add_argument("--list", action="store_true",
                    help="print the names of the tests this would run, and stop")
    ap.add_argument("--tier", default=None, choices=["quick", "full"],
                    help="quick: thirty-six tests, one of each kind of thing, "
                         "for every commit (minutes through tools/qrun.py). "
                         "full: everything, which is what a milestone gets. "
                         "(the campaign is tools/campaign.py: this, over and "
                         "over, on different machines)")
    ap.add_argument("--axis", default=None, metavar="K=V,...",
                    help="what machine to run them on: intc, cpus, mem, hyp, "
                         "vec, vlen, sv. `--axis mem=16G,sv=39'")
    ap.add_argument("--json", default=None, metavar="FILE",
                    help="write the results there as JSON, with what each took")
    ap.add_argument("--no-build", action="store_true",
                    help="the images are already built (a queue runner builds once "
                         "and then starts many of these at the same time; twelve "
                         "makes in one tree would race)")
    args = ap.parse_args()
    if args.axis:
        for kv in args.axis.split(","):
            if not kv.strip():
                continue
            k, _, v = kv.partition("=")
            k = k.strip()
            if k not in ("intc", "cpus", "mem", "hyp", "vec", "vlen", "sv"):
                ap.error("axis: %s is not one of intc, cpus, mem, hyp, vec, vlen, sv" % k)
            AXIS[k] = v.strip()
        if "intc" in AXIS and not args.intc:
            args.intc = AXIS["intc"]
        if "cpus" in AXIS:
            args.cpus = int(AXIS["cpus"])
    modes = [args.intc] if args.intc else MODES
    # TEST_LOG names the console log, so that jobs running side by side
    # do not write over each other's (TEST_FSIMG does the same for the
    # disk image).
    logname = os.environ.get("TEST_LOG", "test-hyp.log")
    log = sys.stdout if args.v else open(os.path.join(HERE, logname), "w")

    global HOST_APPEND
    HOST_APPEND = args.append
    if not (args.no_build or args.list):
        subprocess.run(["make", "-s"], cwd=HERE, check=True)
    import test_hyp_linux
    have_linux = test_hyp_linux.available(HERE)
    if have_linux and not (args.no_build or args.list):
        # the host image carries the Linux kernel and its disk as well
        # (make cannot tell that HOSTFILES changed: start from scratch).
        subprocess.run(["rm", "-f", "fs.img"], cwd=HERE)
        # 192 MB, not the default: the Linux kernel and its disk are
        # 40 MB of it, the three guest images another 48, and the loop
        # test writes 38 MB of its own on top of that.
        subprocess.run(["make", "-s", "fs.img", "HOSTFS_MB=192",
                        "HOSTFILES=linux/Image linux/linux.img"], cwd=HERE, check=True,
                       stdout=subprocess.DEVNULL)
    elif not have_linux and args.suite in ("linux", "all"):
        print("linux/Image not found: run linux/build.sh on a Linux box and linux/fetch.sh; skipping Linux tests")

    failures = 0
    tests = []
    if args.suite in ("native", "all"):
        for m in modes:
            tests.append(("native-%s" % m, lambda m=m: t_native(m, args.cpus, log)))
        # these do not depend on which interrupt controller the machine
        # has, so they are registered once whatever it is -- which is
        # what makes `--axis intc=imsic' run the same thirty-six tests as
        # the baseline rather than the handful whose names carry a
        # controller. (They used to be registered only for plic, and an
        # axis of another controller quietly lost ten of them.)
        if True:
            tests.append(("native-no-hypervisor", lambda: t_native("plic", args.cpus, log, hyp=0)))
            tests.append(("native-no-vector", lambda: t_native("plic", args.cpus, log, vec=0)))
            tests.append(("native-vlen-512", lambda: t_native("plic", args.cpus, log, vlen=512)))
            tests.append(("hostfiles-build", lambda: t_hostfiles(log)))
            tests.append(("clock", lambda: t_clock(log, have_linux)))
            tests.append(("tty", lambda: t_tty(log)))
            tests.append(("libc", lambda: t_libc(log)))
            tests.append(("init", lambda: t_init(log)))
            tests.append(("shutdown", lambda: t_shutdown(log)))
            tests.append(("utils", lambda: t_utils(log)))
            tests.append(("manual", lambda: t_manual(log)))
            tests.append(("logging", lambda: t_logging(log)))
            tests.append(("users", lambda: t_users(log)))
            tests.append(("cowimage", lambda: t_cowimage(log)))
            tests.append(("overlay", lambda: t_overlay(log)))
            tests.append(("mountopts", lambda: t_mountopts(log)))
            tests.append(("sysfs", lambda: t_sysfs(log)))
            tests.append(("aslr", lambda: t_aslr(log)))
            tests.append(("audit", lambda: t_audit(log)))
            tests.append(("ramfs", lambda: t_ramfs(log)))
            tests.append(("fsbench", lambda: t_fsbench(log)))
            tests.append(("blockdev", lambda: t_blockdev(log)))
            tests.append(("loopdev", lambda: t_loopdev(log)))
    if args.suite in ("ns", "all"):
        # the namespaces, one kind at a time
        tests.append(("namespaces", lambda: t_namespaces(log)))
        tests.append(("pidns", lambda: t_pidns(log)))
        tests.append(("mntns", lambda: t_mntns(log)))
        tests.append(("userns", lambda: t_userns(log)))
        tests.append(("mount-privilege", lambda: t_mount_privilege(log)))
    if args.suite in ("ct", "all"):
        # and what is built out of them: groups with limits, a root of
        # its own, a bounded set of system calls, and a network behind a
        # gateway (test_hyp_ct.py)
        tests.append(("cgroups", lambda: t_cgroups(log)))
        tests.append(("containers", lambda: t_containers(log)))
        tests.append(("sysfilter", lambda: t_sysfilter(log)))
        import test_hyp_ct
        tests += test_hyp_ct.tests(modes, args.cpus, log, QEMU, run_cmd)
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
    if args.suite in ("sched", "all"):
        import test_hyp_sched
        tests += test_hyp_sched.tests(modes, args.cpus, log, QEMU)
    if args.suite in ("snap", "all"):
        import test_hyp_snap
        tests += test_hyp_snap.tests(modes, args.cpus, log, QEMU)

    if AXIS.get("hyp") == "0":
        # A machine built without the H extension cannot start a guest, so
        # every test that starts one would fail for the same uninteresting
        # reason -- and while they sat waiting for a vcpu that never comes
        # they held the queue's capacity and took the light jobs behind
        # them down on the clock. What is left is the part of this system
        # that a machine without the extension still has.
        needs_guest = re.compile(r"^(clock|mm-|guest-|linux-|snap-|net-guest|"
                                 r"net-linux|sched-vm-)")
        keep = [t for t in tests if not needs_guest.match(t[0])]
        if len(keep) != len(tests) and not args.list:
            print("(no hypervisor extension on this machine: leaving out %d "
                  "tests that start a guest)" % (len(tests) - len(keep)))
        tests = keep
    if args.tier == "quick":
        # the set for every commit: one of each kind of thing, and
        # §11.3's quarter of an hour is met -- ten minutes on a laptop,
        # one test after another, and a couple through tools/qrun.py.
        # What it leaves out is the long ones -- the 16 GB and 512 GB
        # memory tests, a hundred thousand processes, the Linux guests,
        # the campaign benchmarks -- not whole areas.
        # the tests whose names carry the interrupt controller are named
        # after the one this run is on, so that an axis of intc=imsic
        # gets the same thirty-six tests rather than the handful whose
        # names happen to end in -plic.
        m0 = modes[0]
        quick = (("native-%s|manual|utils|libc|clock|tty|users|sysfs|aslr|"
                  "audit|ramfs|namespaces|pidns|mntns|userns|mount-privilege|"
                  "cgroups|containers|sysfilter|ct-net|ct-exec|"
                  "guest-boot-%s|guest-lifecycle-%s|guest-pages-%s|"
                  "guest-ksm-%s|net-host-%s|net-routes|net-tools|"
                  "mm-1g|proc-8000|sched-fair|sched-group-max|"
                  "snap-save-restore|snap-live|snap-clone|snap-migrate")
                 % (m0, m0, m0, m0, m0, m0))
        tests = [t for t in tests if re.fullmatch(quick, t[0])]
    if args.test:
        # a name, part of one, or a pattern: "-t 'pidns|sysfilter'" runs
        # the two of them and nothing else. A comma-separated list is the
        # same without shell metacharacters (tools/remote.sh passes the
        # arguments through two shells), and an item that is exactly a
        # test's name selects that test alone: "-t snap-migrate,hetero"
        # is snap-migrate and snap-migrate-hetero, not the whole family.
        names = set(t[0] for t in tests)
        def wanted(name):
            for pat in args.test.split(","):
                if pat in names:
                    if pat == name:
                        return True
                elif re.search(pat, name):
                    return True
            return False
        tests = [t for t in tests if wanted(t[0])]
    if args.list:
        # what a queue runner needs to turn a suite into jobs
        for name, _ in tests:
            print(name)
        return 0
    results = []
    started = time.time()
    for name, fn in tests:
        print("== %s" % name)
        t0 = time.time()
        QEMU.live = []
        try:
            fn()
            timeline_report()
            print("== %s: PASS (%.0fs)" % (name, time.time() - t0))
            results.append({"test": name, "result": "pass",
                            "seconds": round(time.time() - t0, 1)})
        except Exception as e:
            failures += 1
            import traceback
            # keep the head of the message as well as the tail: the
            # head says what the test was waiting for, and cutting to
            # the last three thousand characters threw exactly that
            # away and left a page of console with no question attached.
            msg = str(e)
            if len(msg) > 3000:
                head, _, rest = msg.partition("\n")
                msg = head + "\n...\n" + rest[-2800:]
            if not msg.strip():
                msg = traceback.format_exc().strip().splitlines()[-3:]
                msg = " | ".join(l.strip() for l in msg)
            timeline_report()
            print("== %s: FAIL (%.0fs): %s" % (name, time.time() - t0, msg))
            results.append({"test": name, "result": "fail",
                            "seconds": round(time.time() - t0, 1),
                            "why": msg.split("\n")[0][:200]})
    print("%d failure(s)" % failures)
    if args.json:
        # what ran, on what machine, and how long each of it took: a
        # campaign is a directory of these.
        import json
        with open(args.json, "w") as f:
            json.dump({"axis": dict(AXIS), "suite": args.suite,
                       "tier": args.tier, "seconds": round(time.time() - started, 1),
                       "failures": failures, "tests": results}, f, indent=1)
            f.write("\n")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
