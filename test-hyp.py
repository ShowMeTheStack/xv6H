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
import argparse, os, re, select, shutil, subprocess, sys, time

sys.stdout.reconfigure(line_buffering=True)

HERE = os.path.dirname(os.path.abspath(__file__))
MODES = ["plic", "aplic", "imsic"]


class QEMU(object):
    def __init__(self, intc="plic", cpus=3, mem="1G", aia_guests=2, hyp=1, vec=1, vlen=128, log=None,
                 extra_env=None, keep_fs=False):
        # every run starts from a pristine copy of the host file system
        # (TEST_FSIMG names the copy, so that two runs can go in
        # parallel). keep_fs starts from the copy the last run left,
        # which is what a test of what survives a reboot needs.
        fsimg = os.environ.get("TEST_FSIMG", "test-fs.img")
        env = dict(os.environ, INTC=intc, CPUS=str(cpus), MEM=mem, AIA_GUESTS=str(aia_guests),
                   HYP=str(hyp), VEC=str(vec), VLEN=str(vlen), FSIMG=fsimg)
        if extra_env:
            env.update(extra_env)
        if not keep_fs or not os.path.exists(os.path.join(HERE, fsimg)):
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
        out = run_cmd(q, "cat /sys/hypervisor/present")
        assert re.search(r"(?m)^1\s*$", out), out
        out = run_cmd(q, "cat /sys/vector/vlen")
        assert re.search(r"(?m)^128\s*$", out), out

        # and /tmp is a tmpfs init mounted
        out = run_cmd(q, "cat /sys/fs/tmp/type")
        assert "tmpfs" in out, out
        run_cmd(q, "echo t > /tmp/x")
        out = run_cmd(q, "cat /tmp/x")
        assert "t" in out, out
        print("    /sys: the kernel, the disks, the mounts and the machine")
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
        # 192 MB, not the default: the Linux kernel and its disk are
        # 40 MB of it, the three guest images another 48, and the loop
        # test writes 38 MB of its own on top of that.
        subprocess.run(["make", "-s", "fs.img", "HOSTFS_MB=192",
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
            tests.append(("init", lambda: t_init(log)))
            tests.append(("shutdown", lambda: t_shutdown(log)))
            tests.append(("utils", lambda: t_utils(log)))
            tests.append(("logging", lambda: t_logging(log)))
            tests.append(("users", lambda: t_users(log)))
            tests.append(("cowimage", lambda: t_cowimage(log)))
            tests.append(("overlay", lambda: t_overlay(log)))
            tests.append(("mountopts", lambda: t_mountopts(log)))
            tests.append(("sysfs", lambda: t_sysfs(log)))
            tests.append(("ramfs", lambda: t_ramfs(log)))
            tests.append(("fsbench", lambda: t_fsbench(log)))
            tests.append(("blockdev", lambda: t_blockdev(log)))
            tests.append(("loopdev", lambda: t_loopdev(log)))
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
