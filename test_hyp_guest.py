#
# guest tests for test-hyp.py: boot xv6 (and other payloads) as guests of
# xv6 and exercise them. every guest is two files in the host file system.
#
import re, time

DETACH = "\x1d"


def boot_host(QEMU, intc, cpus, log, **kw):
    q = QEMU(intc=intc, cpus=cpus, log=log, **kw)
    q.expect(r"hyp: hypervisor extension present", 40)
    q.expect(r"init: starting sh", 40)
    q.expect(r"\$ ", 10)
    return q


def start_guest(q, args="", timeout=60):
    """vmm start ... (attached) and wait for the xv6 guest's shell prompt."""
    q.send("vmm start %s\n" % args)
    m = q.expect(r"vm (\d+)[^:]*: (\d+) vcpu", 20)
    vmid = int(m.group(1))
    q.expect(r"xv6 kernel is booting", timeout)
    q.expect(r"init: starting sh", timeout)
    q.expect(r"\$ ", 10)
    return vmid


def detach(q):
    q.send(DETACH)
    q.expect(r"\[detached from vm \d+\]", 10)
    q.expect(r"\$ ", 10)


def attach(q, vm):
    q.send("vmm attach %s\n" % vm)
    q.expect(r"\[attached to vm ", 10)


def host_stats(q):
    q.send("vmm stats\n")
    m = q.expect(r"free guest memory: (\d+) MB, ipis: (\d+)", 10)
    q.expect(r"\$ ", 5)
    return int(m.group(1)), int(m.group(2))


def wait_free(q, free0, timeout=20):
    """a killed VM's memory comes back when its last vcpu has left the
    guest, shortly after 'vm killed', and the blocks it wrote to its disk
    are reclaimable only once they have been written. wait for the free
    count to return to the baseline, and return what it reached.

    halfway through the wait the host is told to sync: a dirty block is
    not leaked memory, it is memory that cannot be given back until the
    disk has it, and how long that takes depends on how much the guest
    and the log daemons wrote. asking is more honest than waiting
    longer and hoping."""
    end = time.time() + timeout
    synced = False
    while True:
        free1, _ = host_stats(q)
        if free1 >= free0:
            return free1
        if not synced and time.time() > end - timeout / 2:
            synced = True
            q.send("sync\n")
            q.expect(r"\$ ", 60)
            continue
        if time.time() > end:
            return free1
        time.sleep(0.5)


def vm_ls(q):
    """returns the text of vmm ls."""
    q.send("vmm ls\n")
    q.expect(r"vmm ls", 5)
    m = q.expect(r"(no vms|ID NAME.*?)\n(?=\$ )", 10)
    q.expect(r"\$ ", 5)
    return m.group(1).decode()


def kill_vm(q, vm):
    q.send("vmm kill %s\n" % vm)
    q.expect(r"vm \d+ killed", 20)
    q.expect(r"\$ ", 10)


def assert_gone(q, vmid):
    txt = vm_ls(q)
    assert not re.search(r"^%d  " % vmid, txt, re.M), txt


def t_guest_boot(QEMU, intc, cpus, log):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        free0, _ = host_stats(q)
        vmid = start_guest(q, "-n first")
        q.send("echo guest-says-hi\n")
        q.expect(r"\nguest-says-hi", 10)
        q.expect(r"\$ ", 5)
        q.send("ls /bin\n")   # the programs live in /bin now
        q.expect(r"usertests", 10)
        q.expect(r"\$ ", 5)
        detach(q)
        lst = vm_ls(q)
        assert re.search(r"^0  first +running +1 +128 +/kernel +/disk0.img", lst, re.M), lst
        free1, _ = host_stats(q)
        # guest RAM is backed as the guest touches it: a booted xv6 guest
        # holds a few chunks of its 128 MB plus its 2 MB of page tables.
        assert 2 <= free0 - free1 <= 130, (free0, free1)
        # the same disk cannot be used twice, a name cannot be reused.
        q.send("vmm start -d -f /disk0.img\n")
        q.expect(r"cannot create vm", 10)
        q.expect(r"\$ ", 5)
        q.send("vmm start -d -n first -f /disk1.img\n")
        q.expect(r"already exists", 10)
        q.expect(r"\$ ", 5)
        kill_vm(q, "first")
        assert_gone(q, vmid)
        free2 = wait_free(q, free0)
        # the guest's 128 MB is back. a megabyte of slack: the two
        # failed starts above each took a VM slot, and a slot is kept
        # for reuse rather than freed -- 128 KB that never grows past
        # NVM slots, which is a decision and not a leak.
        assert free0 - free2 <= 1, (free0, free2)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_guest_usertests(QEMU, intc, cpus, log, vcpus=1, extra=""):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        t0 = time.time()
        vmid = start_guest(q, "-c %d %s" % (vcpus, extra))
        m = q.mark()
        q.send("usertests -q\n")
        q.expect(r"ALL TESTS PASSED|FAILED|panic", 1200)
        out = q.output_since(m)
        assert "ALL TESTS PASSED" in out, out[-3000:]
        n = len(re.findall(r"test .*: OK", out))
        print("    guest usertests: %d tests OK in %.0fs" % (n, time.time() - t0))
        q.expect(r"\$ ", 30)
        detach(q)
        q.send("vmm info %d\n" % vmid)
        m = q.expect(r"exits (\d+): mmio (\d+), csr (\d+), wfi sleeps (\d+), injected (\d+)", 10)
        print("    exits %s (mmio %s, csr %s, wfi %s, injected %s)" % tuple(g.decode() for g in m.groups()))
        q.expect(r"\$ ", 5)
        kill_vm(q, vmid)
        assert_gone(q, vmid)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_parallel(QEMU, intc, cpus, log, nguests=3, vcpus=2):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        t0 = time.time()
        ids = []
        for i in range(nguests):
            vmid = start_guest(q, "-n g%d -c %d -f /disk%d.img" % (i, vcpus, i))
            q.send("usertests -q\n")
            q.expect(r"usertests starting", 20)
            detach(q)
            ids.append(vmid)
        print("    %d guests running usertests concurrently" % nguests)
        for i, vmid in enumerate(ids):
            attach(q, "g%d" % i)
            m = q.mark()
            q.expect(r"ALL TESTS PASSED|FAILED|panic", 1500)
            out = q.output_since(m)
            assert "ALL TESTS PASSED" in out, "vm %d: %s" % (vmid, out[-2000:])
            q.expect(r"\$ ", 60)
            detach(q)
            print("    vm %d passed (%.0fs)" % (vmid, time.time() - t0))
        q.send("vmm kill --all\n")
        for vmid in ids:
            q.expect(r"vm \d+ killed", 30)
        q.expect(r"\$ ", 10)
        assert "no vms" in vm_ls(q)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_disk_isolation(QEMU, intc, cpus, log):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        a = start_guest(q, "-n a -f /disk0.img")
        q.send("echo secret-of-vm-a > marker\n")
        q.expect(r"\$ ", 10)
        # the guest's file system is journalled: what it wrote is in its
        # own cache until it syncs, and killing the VM is pulling the
        # power. a guest that wants its data keeps it the way any system
        # does.
        q.send("sync\n")
        q.expect(r"\$ ", 20)
        q.send("cat marker\n")
        q.expect(r"\nsecret-of-vm-a", 10)
        q.expect(r"\$ ", 5)
        detach(q)
        b = start_guest(q, "-n b -f /disk1.img")
        q.send("cat marker\n")
        q.expect(r"cannot open marker", 10)
        q.expect(r"\$ ", 5)
        detach(q)
        kill_vm(q, "a")
        kill_vm(q, "b")
        # the disk file persists: a new VM on /disk0.img sees the file.
        a2 = start_guest(q, "-f /disk0.img")
        q.send("cat marker\n")
        q.expect(r"\nsecret-of-vm-a", 10)
        q.expect(r"\$ ", 5)
        detach(q)
        kill_vm(q, a2)
    finally:
        q.close()


def t_raw_device(QEMU, intc, cpus, log):
    """a guest given a whole block device rather than an image file:
    the host will not mount it while the guest has it, and what the
    guest wrote is on it when the guest is gone."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        # a loop device over a copy of a guest image is a block device
        # like any other, and does not need a second disk in qemu
        q.send("dd if=/disk0.img of=/raw.img bs=65536\n")
        q.expect(r"bytes copied", 120)
        q.expect(r"\$ ", 10)
        q.send("losetup /dev/loop0 /raw.img\n")
        q.expect(r"\$ ", 10)

        vm = start_guest(q, "-n rawg -f /dev/loop0 -m 256 -c 1", 120)
        q.send("echo written-through-the-device > /marker\n")
        q.expect(r"\$ ", 10)
        q.send("sync\n")
        q.expect(r"\$ ", 30)
        detach(q)

        # the host will not mount what a guest is using
        q.send("mkdir /rm\n")
        q.expect(r"\$ ", 10)
        q.send("mount -t ext4 /dev/loop0 /rm\n")
        q.expect(r"in use by a guest", 20)
        q.expect(r"\$ ", 10)
        # and it says so about the device, not a file
        q.send("vmm info rawg\n")
        q.expect(r"disk /dev/loop0 \(\d+ sectors\)", 20)
        q.expect(r"\$ ", 10)

        kill_vm(q, "rawg")
        # now it is the host's again, and what the guest wrote is there
        q.send("mount -t ext4 /dev/loop0 /rm\n")
        q.expect(r"on /rm type ext4", 60)
        q.expect(r"\$ ", 10)
        q.send("cat /rm/marker\n")
        q.expect(r"written-through-the-device", 20)
        q.expect(r"\$ ", 10)
        q.send("umount /rm\n")
        q.expect(r"\$ ", 60)
        q.send("losetup -d /dev/loop0\n")
        q.expect(r"\$ ", 10)
        # an overlay image: two guests from one base, each with its
        # own changes, and the base never written
        q.send("losetup -b /disk0.img /dev/loop1 /g1.cow\n")
        q.expect(r"\$ ", 30)
        q.send("losetup -b /disk0.img /dev/loop2 /g2.cow\n")
        q.expect(r"\$ ", 30)
        v1 = start_guest(q, "-n c1 -f /dev/loop1 -m 128 -c 1", 120)
        q.send("echo guest-one > /mine\n")
        q.expect(r"\$ ", 10)
        q.send("sync\n")
        q.expect(r"\$ ", 30)
        detach(q)
        v2 = start_guest(q, "-n c2 -f /dev/loop2 -m 128 -c 1", 120)
        q.send("cat /mine\n")
        q.expect(r"cannot open", 10)   # the other guest's change is not here
        q.expect(r"\$ ", 10)
        q.send("echo guest-two > /mine\n")
        q.expect(r"\$ ", 10)
        q.send("sync\n")
        q.expect(r"\$ ", 30)
        detach(q)
        kill_vm(q, "c1")
        kill_vm(q, "c2")
        q.send("losetup\n")
        q.expect(r"loop1: \[/disk0.img\] copy-on-write", 20)
        q.expect(r"\$ ", 10)
        q.send("losetup -d /dev/loop1\n")
        q.expect(r"\$ ", 20)
        q.send("losetup -d /dev/loop2\n")
        q.expect(r"\$ ", 20)
        print("    a guest on a raw block device, and two more on one base image")
    finally:
        q.close()


def t_clone(QEMU, intc, cpus, log):
    """cp a disk image in the host, boot from the copy, changes stay in the copy."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        q.send("cp /disk2.img /clone.img\n")
        q.expect(r"\$ ", 120)
        c = start_guest(q, "-n clone -f /clone.img")
        q.send("echo in-the-clone > marker\n")
        q.expect(r"\$ ", 10)
        q.send("sync\n")   # see t_disk_isolation: killing a VM is a power cut
        q.expect(r"\$ ", 20)
        detach(q)
        kill_vm(q, "clone")
        c2 = start_guest(q, "-f /clone.img")
        q.send("cat marker\n")
        q.expect(r"\nin-the-clone", 10)
        q.expect(r"\$ ", 5)
        detach(q)
        kill_vm(q, c2)
        o = start_guest(q, "-f /disk2.img")
        q.send("cat marker\n")
        q.expect(r"cannot open marker", 10)
        q.expect(r"\$ ", 5)
        detach(q)
        kill_vm(q, o)
    finally:
        q.close()


def t_lifecycle(QEMU, intc, cpus, log):
    """names, pause/resume, restart, ps, kill --all."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        start_guest(q, "-n web -c 2")
        detach(q)
        q.send("vmm start -n db -f /disk1.img -d\n")
        q.expect(r"vm 1 db: 1 vcpu", 10)
        q.expect(r"\$ ", 5)
        lst = vm_ls(q)
        assert re.search(r"^0  web +running +2", lst, re.M) and re.search(r"^1  db +running +1", lst, re.M), lst
        # pause: the guest stops responding; resume: it answers.
        q.send("vmm pause web\n")
        q.expect(r"vm 0 paused", 10)
        q.expect(r"\$ ", 5)
        assert re.search(r"^0  web +paused", vm_ls(q), re.M)
        attach(q, "web")
        q.send("echo while-paused\n")
        try:
            q.expect(r"\nwhile-paused", 3)
            raise AssertionError("paused guest answered")
        except TimeoutError:
            pass
        detach(q)
        q.send("vmm resume web\n")
        q.expect(r"vm 0 resumed", 10)
        q.expect(r"\$ ", 5)
        attach(q, "web")
        q.expect(r"while-paused", 10) # the typed line is processed now
        q.expect(r"\$ ", 10)
        q.send("echo after-resume\n")
        q.expect(r"after-resume\r?\n", 10)
        q.expect(r"\$ ", 10)
        detach(q)
        # ps shows the guest's processes
        q.send("vmm ps db\n")
        q.expect(r"sleep +init", 10)
        q.expect(r"\$ ", 10)
        # restart keeps the configuration
        q.send("vmm restart db\n")
        q.expect(r"vm 1 killed", 20)
        q.expect(r"vm (\d+) db: 1 vcpu\(s\), 128 MB, kernel /kernel, disk /disk1.img", 10)
        q.expect(r"\$ ", 5)
        attach(q, "db")
        q.expect(r"init: starting sh", 30)
        detach(q)
        q.send("vmm kill --all\n")
        q.expect(r"vm \d+ killed", 20)
        q.expect(r"vm \d+ killed", 20)
        q.expect(r"\$ ", 10)
        assert "no vms" in vm_ls(q)
        q.send("vmm kill --all\n")
        q.expect(r"no vms", 10)
        q.expect(r"\$ ", 5)
    finally:
        q.close()


def t_hello(QEMU, intc, cpus, log):
    """a guest that is not xv6: ELF at 0x80400000 with 3 vcpus, then the same
    code as a raw binary."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        free0, _ = host_stats(q)
        for args in ("-k /hello -f none -c 3", "-k /hello.bin -f none -r 0x80400000 -c 3"):
            q.send("vmm start %s\n" % args)
            q.expect(r"vm (\d+): 3 vcpu", 10)
            q.expect(r"hello: running at 0x0000000080400000, hart 0, dtb at 0x0000000087ff0000", 20)
            q.expect(r"hello: device tree ok, \d+ bytes", 5)
            q.expect(r"hello: sbi console works", 5)
            q.expect(r"hello: sbi spec version call returned 0", 5)
            q.expect(r"hello: 3 hart\(s\) total", 10)
            q.expect(r"hello: done", 5)
            # the guest shut itself down through SBI: vmm returns to the shell.
            q.expect(r"\[vm \d+ has exited\]", 20)
            q.expect(r"\$ ", 20)
            assert "no vms" in vm_ls(q)
        free1 = wait_free(q, free0)
        assert free1 == free0, (free0, free1)
        # loading outside guest RAM is refused
        q.send("vmm start -k /hello.bin -f none -r 0x10000000 -d\n")
        q.expect(r"cannot create vm", 10)
        q.expect(r"\$ ", 5)
        q.send("vmm start -k /nonexistent -d\n")
        q.expect(r"cannot create vm", 10)
        q.expect(r"\$ ", 5)
    finally:
        q.close()


def t_fdv_guests(QEMU, intc, cpus, log, vec=1):
    """bare-metal guests using the FPU and the vector unit (state across
    vcpu exits); with VEC=0 the vector guest must notice the absence."""
    q = boot_host(QEMU, intc, cpus, log, vec=vec)
    try:
        q.send("vmm start -k /fdguest -f none\n")
        q.expect(r"fdguest: basel\*10000 = 16448", 30)
        q.expect(r"fdguest: after wfi fs0\*1000 = 12345678", 10)
        q.expect(r"fdguest: ok", 10)
        q.expect(r"\[vm \d+ has exited\]", 20)
        q.expect(r"\$ ", 20)
        q.send("vmm start -k /vguest -f none\n")
        if vec:
            q.expect(r"vguest: vlen \d+ bits", 30)
            q.expect(r"vguest: vadd ok", 10)
            q.expect(r"vguest: state across wfi ok", 10)
            q.expect(r"vguest: ok", 10)
        else:
            q.expect(r"vguest: no vector extension", 30)
        q.expect(r"\[vm \d+ has exited\]", 20)
        q.expect(r"\$ ", 20)
        assert "no vms" in vm_ls(q)
        # the standalone programs, on the host and in a guest
        for prog in ("fpbench 3", "vecbench 3", "mandel q"):
            q.send(prog + "\n")
            q.expect(r"(fpbench|vecbench|mandel): (ok|no vector extension)", 60)
            q.expect(r"\$ ", 10)
        vmid = start_guest(q)
        for prog in ("fpbench 2", "vecbench 2", "mandel q"):
            q.send(prog + "\n")
            q.expect(r"(fpbench|vecbench|mandel): (ok|no vector extension)", 90)
            q.expect(r"\$ ", 10)
        detach(q)
        kill_vm(q, vmid)
    finally:
        q.close()


def t_malicious(QEMU, intc, cpus, log):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        free0, _ = host_stats(q)
        q.send("vmm start -k /badguest -f none\n")
        q.expect(r"vm (\d+): 1 vcpu", 20)
        q.expect(r"badguest: hello", 20)
        q.expect(r"badguest: done, shutting down", 30)
        q.expect(r"\[vm \d+ has exited\]", 20)
        q.expect(r"\$ ", 20)
        assert "no vms" in vm_ls(q)
        free1 = wait_free(q, free0)
        assert free1 == free0, (free0, free1)
        vmid = start_guest(q)
        q.send("echo still-fine\n")
        q.expect(r"\nstill-fine", 10)
        detach(q)
        kill_vm(q, vmid)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_churn(QEMU, intc, cpus, log, rounds=4):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        free0, _ = host_stats(q)
        for r in range(rounds):
            vmid = start_guest(q, "-c 2")
            q.send("echo round-%d\n" % r)
            q.expect(r"\nround-%d" % r, 10)
            detach(q)
            kill_vm(q, vmid)
        free1 = wait_free(q, free0)
        assert free1 == free0, (free0, free1)
    finally:
        q.close()


def tests(modes, cpus, log, QEMU, usertests_ok):
    t = []
    for m in modes:
        t.append(("guest-boot-%s" % m, lambda m=m: t_guest_boot(QEMU, m, cpus, log)))
        t.append(("guest-usertests-%s" % m, lambda m=m: t_guest_usertests(QEMU, m, cpus, log)))
        t.append(("guest-smp-usertests-%s" % m, lambda m=m: t_guest_usertests(QEMU, m, cpus, log, 2)))
        if m == "imsic":
            t.append(("guest-smp-usertests-imsic-soft", lambda m=m: t_guest_usertests(QEMU, m, cpus, log, 2, "-s")))
        t.append(("guest-parallel-%s" % m, lambda m=m: t_parallel(QEMU, m, cpus, log)))
        t.append(("guest-disk-isolation-%s" % m, lambda m=m: t_disk_isolation(QEMU, m, cpus, log)))
        t.append(("guest-clone-%s" % m, lambda m=m: t_clone(QEMU, m, cpus, log)))
        t.append(("guest-raw-device-%s" % m, lambda m=m: t_raw_device(QEMU, m, cpus, log)))
        t.append(("guest-lifecycle-%s" % m, lambda m=m: t_lifecycle(QEMU, m, cpus, log)))
        t.append(("guest-hello-%s" % m, lambda m=m: t_hello(QEMU, m, cpus, log)))
        t.append(("guest-churn-%s" % m, lambda m=m: t_churn(QEMU, m, cpus, log)))
        t.append(("guest-malicious-%s" % m, lambda m=m: t_malicious(QEMU, m, cpus, log)))
        t.append(("guest-fd-vector-%s" % m, lambda m=m: t_fdv_guests(QEMU, m, cpus, log)))
    t.append(("guest-fd-vector-novec-plic", lambda: t_fdv_guests(QEMU, "plic", cpus, log, vec=0)))
    # host configurations: a single hart, and more harts than the default.
    t.append(("guest-smp-on-1-hart-plic", lambda: t_guest_usertests(QEMU, "plic", 1, log, 2)))
    t.append(("guest-parallel-on-4-harts-imsic", lambda: t_parallel(QEMU, "imsic", 4, log, 3, 2)))
    return t
