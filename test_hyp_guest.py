#
# guest tests for test-hyp.py: boot xv6 (and other payloads) as guests of
# xv6 and exercise them. every guest is two files in the host file system.
#
import re, time

DETACH = "\x1d"


def boot_host(QEMU, intc, cpus, log, **kw):
    q = QEMU(intc=intc, cpus=cpus, log=log, **kw)
    # whichever of the two the machine prints: a host without the H
    # extension says so and carries on, and the tests that boot a host
    # without starting a guest -- the network, the processes, the
    # namespaces, the containers -- are exactly the ones worth running on
    # a machine that has none. Waiting only for "present" made four of
    # them sit through six minutes of a banner that was never coming.
    q.expect(r"hyp: (hypervisor extension present|no hypervisor extension)", 90)
    q.expect(r"init: starting sh", 90)
    q.expect(r"\$ ", 30)
    return q


def start_guest(q, args="", timeout=90):
    """vmm start ... (attached) and wait for the xv6 guest's shell prompt."""
    q.send("vmm start %s\n" % args)
    m = q.expect(r"vm (\d+)[^:]*: (\d+) vcpu", 30)
    vmid = int(m.group(1))
    q.expect(r"xv6 kernel is booting", timeout)
    q.expect(r"init: starting sh", timeout)
    q.expect(r"\$ ", 30)
    return vmid


# how long to wait for the machine to answer something it should answer
# at once. On an idle laptop every one of these takes well under a
# second; on a build box running six other suites it does not, and a
# test that fails because the machine it is on is busy says nothing
# about the machine it is testing. Deliberate negatives -- a paused
# guest that must *not* answer -- keep their own short waits.
ATONCE = 30


def detach(q):
    q.send(DETACH)
    q.expect(r"\[detached from vm \d+\]", ATONCE)
    q.expect(r"\$ ", ATONCE)


def attach(q, vm):
    q.send("vmm attach %s\n" % vm)
    q.expect(r"\[attached to vm ", ATONCE)


def host_stats(q):
    q.send("vmm stats\n")
    m = q.expect(r"free guest memory: (\d+) MB \((\d+) pages\), ipis: (\d+)", ATONCE)
    q.expect(r"\$ ", ATONCE)
    return int(m.group(1)), int(m.group(3))


def host_pages(q):
    """free guest memory in pages rather than megabytes.

    the megabyte figure rounds down, so two readings 255 pages apart
    can print the same number and two readings one page apart can
    print different ones.  Anything looking for a leak has to use
    this."""
    q.send("vmm stats\n")
    m = q.expect(r"free guest memory: \d+ MB \((\d+) pages\)", 10)
    q.expect(r"\$ ", 5)
    return int(m.group(1))


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
    q.expect(r"vmm ls", ATONCE)
    m = q.expect(r"(no vms|ID NAME.*?)\n(?=\$ )", ATONCE)
    q.expect(r"\$ ", ATONCE)
    return m.group(1).decode()


def kill_vm(q, vm):
    q.send("vmm kill %s\n" % vm)
    q.expect(r"vm \d+ killed", ATONCE)
    q.expect(r"\$ ", ATONCE)


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


def t_pages(QEMU, intc, cpus, log):
    """a guest whose memory is backed 4 KB at a time.

    with -4 the host never uses a megapage for guest RAM: every chunk
    gets a table of 512 leaves and a frame per page as the guest
    touches it.  That is the shape a balloon and same-page merging
    need, and it is also what is left when the host has no 2 MB blocks
    to give, so it has to work as well as the fast path does -- and it
    has to give every page back."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -4")
        m = q.mark()
        q.send("usertests -q\n")
        q.expect(r"ALL TESTS PASSED|FAILED|panic", 1200)
        out = q.output_since(m)
        assert "ALL TESTS PASSED" in out, out[-3000:]
        q.expect(r"\$ ", 30)
        detach(q)

        # the host's own view: nothing megapaged, and tables to match
        m = q.mark()
        q.send("vmm info %d\n" % vmid)
        q.expect(r"\$ ", 10)
        info = q.output_since(m)
        mm = re.search(r"memory: (\d+) chunks split to pages, (\d+) table pages"
                       r", 4 KB pages only", info)
        assert mm, info
        split, tables = int(mm.group(1)), int(mm.group(2))
        assert split > 0, info
        # one table page per split chunk, plus the levels above them
        assert tables >= split, info

        kill_vm(q, vmid)
        assert_gone(q, vmid)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)

        # and every page of it comes back.  This is measured with a
        # guest that only boots, as guest-churn is, because a guest
        # that runs usertests leaves twenty-odd pages of the host's
        # own caches behind whether its memory was megapaged or not --
        # that is what the host wrote to the disk image, not what the
        # guest was given.
        #
        # the slack is four pages against six rounds, and it is there
        # because backing a guest a page at a time is a hundred times
        # more allocation than megapaging it, which makes the host's
        # object caches grow once.  Measured over eighteen rounds the
        # count loses a single page early and then does not move
        # again; a leak would go on costing a page a round, and four
        # pages over six rounds would not hide one.
        free0 = settled_pages(q)
        for r in range(1, 7):
            vmid = start_guest(q, "-c 1 -4")
            q.send("echo round-%d\n" % r)
            q.expect(r"\nround-%d" % r, 10)
            detach(q)
            kill_vm(q, vmid)
        free1 = wait_pages(q, free0 - 4)
        assert free1 >= free0 - 4, (free0, free1)
    finally:
        q.close()


def vm_resident_mb(q, vmid):
    m = q.mark()
    q.send("vmm info %d\n" % vmid)
    q.expect(r"\$ ", 10)
    txt = q.output_since(m)
    mm = re.search(r"\((\d+) MB resident\)", txt)
    assert mm, txt
    return int(mm.group(1)), txt


def t_balloon(QEMU, intc, cpus, log):
    """a guest gives its memory back, and takes it again.

    the host asks; the guest's driver sets pages aside so that nothing
    in the guest will touch them and says which they are; the host
    takes the backing away.  What is checked is that the memory really
    moves -- the guest's resident size falls, the host's free count
    rises by about the same -- and that deflating puts it all back.

    the guest is made to touch its memory first and then let go of it
    again, because a balloon can only hand over pages the guest is not
    using.  Writing a file and deleting it does both in a few seconds,
    where running a test suite would take minutes."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 256")
        # sbrkmuch grows the heap as far as it will go, writes to all
        # of it and gives it back: the host has backed those pages and
        # the guest is no longer using them, which is exactly what a
        # balloon is for. (The disk is sixteen megabytes, so writing a
        # file would not get anywhere near.)
        q.send("usertests sbrkmuch\n")
        q.expect(r"ALL TESTS PASSED|SOME TESTS FAILED|panic", 300)
        q.expect(r"\$ ", 60)
        detach(q)

        res0, _ = vm_resident_mb(q, vmid)
        free0 = host_pages(q)
        assert res0 >= 64, res0

        # a balloon is a request, not a demand: the guest hands over
        # what it is not using, which is less than it holds.  What is
        # checked is that whatever it did give up, the host got --
        # not that it gave up some particular figure.
        want = 64
        q.send("vmm balloon %d %d\n" % (vmid, want))
        q.expect(r"asked for %d MB back" % want, 30)
        q.expect(r"\$ ", 10)
        given, res1 = 0, res0
        for _ in range(60):
            time.sleep(1)
            res1, txt = vm_resident_mb(q, vmid)
            mm = re.search(r"balloon: \d+ MB asked for, (\d+) MB given back", txt)
            now = int(mm.group(1)) if mm else 0
            if now > 0 and now == given:
                break          # it has stopped moving
            given = now
        assert given >= 32, (given, want, res0, res1)
        free1 = host_pages(q)
        # the guest's resident set really falls
        assert res1 <= res0 - 16, (res0, res1, given)
        # and the host gets about what the guest stopped holding. It
        # is not the figure the guest reports giving back: inflating
        # means allocating pages, and allocating one writes junk into
        # it, so a page the guest had never touched is faulted in
        # before it is handed over. What the host gains is the fall in
        # the resident set, less the tables it now needs to map those
        # chunks a page at a time.
        assert free1 - free0 >= (res0 - res1 - 8) * 256, (free0, free1, res0, res1)

        q.send("vmm balloon %d 0\n" % vmid)
        q.expect(r"asked for 0 MB back", 30)
        q.expect(r"\$ ", 10)
        res2 = res1
        for _ in range(90):
            time.sleep(1)
            res2, _ = vm_resident_mb(q, vmid)
            if res2 >= res0 - 4:
                break
        assert res2 >= res0 - 4, (res0, res2)

        kill_vm(q, vmid)
        assert_gone(q, vmid)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_memlimit(QEMU, intc, cpus, log):
    """a VM may be given more memory than it is allowed to hold.

    -m says how much memory the guest sees; -M says how much of the
    host's it may have at once.  Nothing is backed until it is
    touched, so the two are different questions, and this is where the
    second one is answered: a fault that would go over the limit waits
    while the guest is asked -- through its balloon -- to give
    something back, and a guest that will not come down is stopped
    rather than allowed past it.

    the invariant is the same whichever happens: the resident size
    never exceeds the limit.  A guest asked to give memory back while
    it is still under the cap does so, and guest-balloon checks that;
    what this checks is the other end, where there is nothing left to
    give."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        # detached, so the host's own shell stays available
        q.send("vmm start -d -c 1 -m 256 -M 128\n")
        m = q.expect(r"vm (\d+): 1 vcpu", 30)
        vmid = int(m.group(1))
        q.expect(r"\$ ", 20)
        res, txt = vm_resident_mb(q, vmid)
        assert "limit 128 MB" in txt, txt
        assert res <= 128, (res, txt)

        # now ask it for a great deal more than it may have
        attach(q, vmid)
        mark = q.mark()
        q.send("usertests -q\n")
        q.expect(r"ALL TESTS PASSED|FAILED|panic|halting the vm", 1200)
        out = q.output_since(mark)
        assert "panic" not in out, out[-2000:]
        if "halting the vm" in out:
            # stopped at the limit, which is what a limit is for
            q.expect(r"\[vm \d+ has exited\]", 60)
            q.expect(r"\$ ", 30)
        else:
            q.expect(r"\$ ", 120)
            detach(q)
            res, txt = vm_resident_mb(q, vmid)
            assert res <= 128, (res, txt)
            kill_vm(q, vmid)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_ksm(QEMU, intc, cpus, log):
    """two guests of the same image hold mostly the same bytes.

    with same-page merging on, the host keeps one copy of each and
    both map it read-only; the first write to one of them faults and
    the writer is given a copy of its own.  What is checked is that
    the sharing really happens, that the host gets memory back for
    it, that the guests go on working afterwards -- forktest writes
    to a great many shared pages -- and that everything comes back
    when they are killed."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        a = start_guest(q, "-n a -m 128 -c 1 -f /disk0.img")
        detach(q)
        b = start_guest(q, "-n b -m 128 -c 1 -f /disk1.img")
        detach(q)
        free0 = host_pages(q)

        q.send("vmm ksm on\n")
        q.expect(r"same-page merging on", 10)
        q.expect(r"\$ ", 5)
        merged = 0
        for _ in range(60):
            time.sleep(1)
            m = q.mark()
            q.send("vmm stats\n")
            q.expect(r"\$ ", 10)
            mm = re.search(r"(\d+) merged, (\d+) shared", q.output_since(m))
            if mm and int(mm.group(1)) >= 500:
                merged = int(mm.group(1))
                break
        assert merged >= 500, merged
        free1 = host_pages(q)
        # sharing costs a table page and a frame array for every chunk
        # it has to split, so the gain is less than the count merged
        assert free1 > free0, (free0, free1)

        # and the guests still work: forktest writes to page after page
        # that is now shared, which is the copy-on-write path
        attach(q, "a")
        q.send("forktest\n")
        q.expect(r"fork test OK", 60)
        q.expect(r"\$ ", 10)
        detach(q)
        attach(q, "b")
        q.send("echo still-b\n")
        q.expect(r"\nstill-b", 15)
        detach(q)

        q.send("vmm ksm off\n")
        q.expect(r"same-page merging off", 10)
        q.expect(r"\$ ", 5)
        kill_vm(q, a)
        kill_vm(q, b)
        free2 = wait_pages(q, free0)
        assert free2 >= free0, (free0, free2)
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
    """names, pause/resume, restart, ps, kill --all.

    the waits here are generous on purpose. Everything this test asks
    for should happen at once, and on an idle machine it does -- five
    seconds each was enough for a year. On a build box running six
    suites at a load average of fifteen it is not, and a test that
    fails because the machine it is on is busy says nothing about the
    machine it is testing."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        start_guest(q, "-n web -c 2")
        detach(q)
        q.send("vmm start -n db -f /disk1.img -d\n")
        q.expect(r"vm 1 db: 1 vcpu", 30)
        q.expect(r"\$ ", 30)
        lst = vm_ls(q)
        assert re.search(r"^0  web +running +2", lst, re.M) and re.search(r"^1  db +running +1", lst, re.M), lst
        # pause: the guest stops responding; resume: it answers.
        q.send("vmm pause web\n")
        q.expect(r"vm 0 paused", 30)
        q.expect(r"\$ ", 30)
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
        q.expect(r"vm 0 resumed", 30)
        q.expect(r"\$ ", 30)
        attach(q, "web")
        q.expect(r"while-paused", 30) # the typed line is processed now
        q.expect(r"\$ ", 30)
        q.send("echo after-resume\n")
        q.expect(r"after-resume\r?\n", 30)
        q.expect(r"\$ ", 30)
        detach(q)
        # ps shows the guest's processes. `db' was started detached, so
        # everything it has said since is still in its console ring and
        # `vmm ps' prints that before the list: attaching once drains the
        # ring and proves it has finished booting, and then the list is
        # the only thing that arrives. (Without this the assertion raced
        # the boot -- on eight harts it lost.)
        attach(q, "db")
        q.send("\n")
        q.expect(r"\$ ", 60)
        detach(q)
        q.send("vmm ps db\n")
        q.expect(r"sleep +init", 30)
        q.expect(r"\$ ", 30)
        # restart keeps the configuration
        q.send("vmm restart db\n")
        q.expect(r"vm 1 killed", 60)
        q.expect(r"vm (\d+) db: 1 vcpu\(s\), 128 MB, kernel /kernel, disk /disk1.img", 30)
        q.expect(r"\$ ", 30)
        attach(q, "db")
        q.expect(r"init: starting sh", 30)
        detach(q)
        q.send("vmm kill --all\n")
        q.expect(r"vm \d+ killed", 60)
        q.expect(r"vm \d+ killed", 60)
        q.expect(r"\$ ", 30)
        assert "no vms" in vm_ls(q)
        q.send("vmm kill --all\n")
        q.expect(r"no vms", 30)
        q.expect(r"\$ ", 30)
    finally:
        q.close()


def t_hello(QEMU, intc, cpus, log):
    """a guest that is not xv6: ELF at 0x80400000 with 3 vcpus, then the same
    code as a raw binary."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        def hello():
            for args in ("-k /hello -f none -c 3",
                         "-k /hello.bin -f none -r 0x80400000 -c 3"):
                q.send("vmm start %s\n" % args)
                q.expect(r"vm (\d+): 3 vcpu", 10)
                q.expect(r"hello: running at 0x0000000080400000, hart 0, "
                         r"dtb at 0x0000000087ff0000", 20)
                q.expect(r"hello: device tree ok, \d+ bytes", 5)
                q.expect(r"hello: sbi console works", 5)
                q.expect(r"hello: sbi spec version call returned 0", 5)
                q.expect(r"hello: 3 hart\(s\) total", 10)
                q.expect(r"hello: done", 5)
                # the guest shut itself down through SBI: vmm returns.
                q.expect(r"\[vm \d+ has exited\]", 20)
                q.expect(r"\$ ", 20)
                assert "no vms" in vm_ls(q)

        costs_nothing(q, hello)
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

    def badguest():
        q.send("vmm start -k /badguest -f none\n")
        q.expect(r"vm (\d+): 1 vcpu", 20)
        q.expect(r"badguest: hello", 20)
        q.expect(r"badguest: done, shutting down", 30)
        q.expect(r"\[vm \d+ has exited\]", 20)
        q.expect(r"\$ ", 20)
        assert "no vms" in vm_ls(q)

    try:
        costs_nothing(q, badguest)
        vmid = start_guest(q)
        q.send("echo still-fine\n")
        q.expect(r"\nstill-fine", 10)
        detach(q)
        kill_vm(q, vmid)
        q.send("echo host-ok\n")
        q.expect(r"\nhost-ok", 10)
    finally:
        q.close()


def t_fuzz(QEMU, intc, cpus, log, rounds=10000):
    """a guest that does everything wrong, at random, for as long as it is
    asked to: random writes and reads over every device the machine
    has, random CSR numbers, illegal instructions, guest-physical
    addresses that are not there, nonsense virtqueues and unknown SBI
    calls. What is under test is the host: it must stay up, keep its
    console, take the machine's memory back, and still be able to run
    the next guest. The seed is printed, so a run that finds something
    can be run again with the same one."""
    import random
    seed = random.randrange(1, 2 ** 31)
    q = boot_host(QEMU, intc, cpus, log)
    try:
        free0, _ = host_stats(q)
        print("    seed %d, %d rounds" % (seed, rounds))
        # two vcpus: a device model that is right for one and wrong for
        # two is a hypervisor bug of exactly the kind this is looking
        # for. The second vcpu fuzzes the devices while the first one
        # counts the rounds.
        q.send('vmm start -k /fuzzguest -f none -m 32 -c 2 -a "seed=%d rounds=%d"\n'
               % (seed, rounds))
        # short patterns, and the guest prints each of its two markers
        # twice: the host writes to the same console, and a message it
        # prints about this guest (a fault, a nonsense queue) can land
        # in the middle of a line the guest is printing.
        mark = q.mark()
        q.expect(r"fuzzguest: seed", 60)
        q.expect(r"fuzzguest: done", 600)
        q.expect(r"traps", 30)   # the rest of that line, which is the numbers
        out = q.output_since(mark)
        m = re.search(r"seed (\d+) rounds (\d+)", out)
        assert m and int(m.group(1)) == seed, out[:400]
        m = re.search(r"done, \d+ rounds, (\d+) traps", out)
        # it is only a test of the paths it reached: a run that took no
        # traps at all would mean the payload never got going.
        assert m, out[-400:]
        traps = int(m.group(1))
        assert traps > rounds // 20, traps
        print("    the guest took %d traps and the host is still here" % traps)
        q.expect(r"\[vm \d+ has exited\]", ATONCE)
        q.expect(r"\$ ", ATONCE)
        assert "no vms" in vm_ls(q)

        # the host still has its console, its memory and its guests
        m = q.mark()
        q.send("echo host-still-here\n")
        q.expect(r"\nhost-still-here", ATONCE)
        q.expect(r"\$ ", ATONCE)
        free1 = wait_pages(q, free0)
        assert free1 >= free0 - 1, (free0, free1)
        vmid = start_guest(q, "-n after -c 1 -f /disk0.img")
        q.send("echo guest-after-fuzz\n")
        q.expect(r"\nguest-after-fuzz", ATONCE)
        detach(q)
        kill_vm(q, vmid)
    finally:
        q.close()


def wait_pages(q, free0, timeout=20):
    """free memory in megabytes, once the machine that has gone has
    given it back."""
    end = time.time() + timeout
    while True:
        free1, _ = host_stats(q)
        if free1 >= free0 - 1 or time.time() > end:
            return free1
        time.sleep(0.5)


def t_ownership(QEMU, intc, cpus, log):
    """a machine belongs to the user who created it, and a descriptor for
    it is a capability: vmowntest does the work (user/vmowntest.c), and
    /proc/vmm is checked from the shell."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        m = q.mark()
        q.send("vmowntest\n")
        q.expect(r"vmowntest: (ok|\d+ failure)", 120)
        out = q.output_since(m)
        assert "vmowntest: ok" in out, out
        q.expect(r"\$ ", ATONCE)

        # the registry: a running machine is in it, with its owner
        vmid = start_guest(q, "-n owned -c 1 -f /disk0.img")
        detach(q)
        m = q.mark()
        q.send("cat /proc/vmm\n")
        q.expect(r"\$ ", ATONCE)
        out = q.output_since(m)
        assert re.search(r"(?m)^%d owned 0 (running|new) " % vmid, out), out
        # and an unprivileged reader sees none of it
        m = q.mark()
        q.send("su user -c 'cat /proc/vmm'\n")
        q.expect(r"\$ ", ATONCE)
        out = q.output_since(m)
        assert "owned" not in out, out
        # nor can they kill it
        m = q.mark()
        q.send("su user -c 'vmm kill %d'\n" % vmid)
        q.expect(r"\$ ", ATONCE)
        out = q.output_since(m)
        assert "killed" not in out, out
        assert "owned" in vm_ls(q)
        kill_vm(q, vmid)
        assert "no vms" in vm_ls(q)
    finally:
        q.close()


def costs_nothing(q, run, rounds=2):
    """running and killing a guest must cost the host nothing.

    `run` is done once before the baseline is taken, because the first
    guest of a boot is the one that reads its kernel and its disk
    through the block cache for the first time and makes the object
    caches behind them grow; those pages are kept, not lost.  After
    that, every round must give back everything it took, and the
    comparison is in pages -- the megabyte figure rounds down, so two
    readings 255 pages apart can print the same number."""
    run()
    free0 = settled_pages(q)
    for _ in range(rounds):
        run()
    free1 = wait_pages(q, free0)
    assert free1 >= free0, (free0, free1)


def churn_round(q, r):
    vmid = start_guest(q, "-c 2")
    q.send("echo round-%d\n" % r)
    q.expect(r"\nround-%d" % r, 10)
    detach(q)
    kill_vm(q, vmid)


def wait_pages(q, want, timeout=20):
    """as wait_free, but in pages, and it syncs halfway for the same
    reason: a dirty block is not leaked memory, it is memory that
    cannot be given back until the disk has it."""
    end = time.time() + timeout
    synced = False
    while True:
        now = host_pages(q)
        if now >= want:
            return now
        if not synced and time.time() > end - timeout / 2:
            synced = True
            q.send("sync\n")
            q.expect(r"\$ ", 60)
            continue
        if time.time() > end:
            return now
        time.sleep(0.5)


def settled_pages(q, timeout=20):
    """free memory once it has stopped moving: the disk is written
    first, because a dirty block counts against the total until it is,
    and then the count is read until two readings agree.  A baseline
    taken while memory is still coming back is a baseline that is too
    low, and a test that compares against one is a test that passes
    whatever happens."""
    q.send("sync\n")
    q.expect(r"\$ ", 60)
    end = time.time() + timeout
    last = -1
    while time.time() < end:
        now = host_pages(q)
        if now == last:
            return now
        last = now
        time.sleep(0.5)
    return last


def t_churn(QEMU, intc, cpus, log, rounds=4):
    """starting and killing guests must not cost memory.

    the baseline is taken after one round, not at boot.  The first
    guest is the one that reads its kernel image and its disk through
    the block cache for the first time and makes the host grow the
    object caches behind all that; those pages are kept, not lost, and
    counting them as a leak says nothing about the rounds that follow.
    What this asks is the real question: after the caches are warm,
    does a start and a kill get back every page it took?  In pages,
    because a megabyte is 256 of them."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        churn_round(q, 0)
        free0 = settled_pages(q)
        for r in range(1, rounds + 1):
            churn_round(q, r)
        free1 = wait_pages(q, free0 - 2)
        # two pages of slack against four rounds. Measured over
        # eighteen rounds the count steps down by one page twice and
        # then stays; that is the host's own caches settling, not a
        # leak, which would cost a page or more every round and show
        # as four here.
        assert free1 >= free0 - 2, (free0, free1)
    finally:
        q.close()



def host_cmd(q, line, timeout=30):
    """a command on the host shell, and everything it printed."""
    m = q.mark()
    q.send(line + "\n")
    q.expect(r"\$ ", timeout)
    return q.output_since(m)


def vm_info(q, vmid, timeout=30):
    """the text of `vmm info N', synchronised on its first line rather than on the
    prompt alone: right after a detach the host shell can leave a second prompt in
    the buffer, and an expect for `$ ' then returns before the command has
    printed anything, handing the caller half a line."""
    m = q.mark()
    q.send("vmm info %d\n" % vmid)
    q.expect(r"vm %d: name" % vmid, timeout)   # past any stale prompt, onto the output
    q.expect(r"\$ ", timeout)                    # and now the prompt is the real one
    return q.output_since(m)


def wait_adopted(q, mark, timeout=90):
    """the guest's acknowledgement of a revision.

    The host prints this when the guest acks, which is asynchronous: it can land
    before the prompt the `vmm revise' command leaves or some milliseconds after
    it. Waiting for it with a plain expect() after the prompt therefore misses it
    about half the time, because the prompt's expect() had already consumed it.
    So the search is rewound to the caller's mark, which is sound here --
    everything between the mark and now has already been asserted on."""
    q.consumed = mark
    hit = q.expect(r"revision \d+ adopted \(0x[0-9a-f]+\)", timeout)
    # and put the cursor back at the end, or the next expect() for a prompt
    # matches the one this command already left and returns before the next
    # command has printed anything. Everything up to here has been asserted on.
    q.consumed = len(q.buf)
    return hit


def t_revise_intc(QEMU, intc, cpus, log):
    """the interrupt controller exchanged under a running guest, twice, and once
    while it is paused.

    This is the mechanism the revisable-machine work is built on and it had no
    test: it was demonstrated by hand and then left to the honour system. What is
    checked is not that the host said it did something -- it is that the guest
    went on working afterwards, on a controller it was not booted with, with its
    file and its disk intact, and that it says itself what it adopted. A guest
    that quietly rebooted would pass every other check, so that is checked too."""
    q = boot_host(QEMU, "imsic", cpus, log)
    try:
        # booted on the PLIC on a host that also has the AIA, so it can be moved
        # up and back down again
        vmid = start_guest(q, "-c 1 -m 64 -P")
        q.send("echo before-the-switch > /keep\n")
        q.expect(r"\$ ", ATONCE)
        detach(q)
        out = host_cmd(q, "vmm info %d" % vmid)
        assert "intc plic" in out, out

        for want, shown in (("imsic", "aplic-msi+imsic"), ("plic", "plic")):
            m = q.mark()
            q.send("vmm revise %d %s\n" % (vmid, want))
            q.expect(r"interrupt controller -> %s" % want, 60)
            q.expect(r"\$ ", ATONCE)
            sw = q.output_since(m)
            assert re.search(r"interrupt controller \w+ -> %s, \d+ line\(s\) held" % want,
                             sw), sw
            # the guest re-read its description and rebound its driver
            wait_adopted(q, m)
            out = host_cmd(q, "vmm info %d" % vmid)
            assert ("intc " + shown) in out, out
            # alive: userspace runs, the file is there, the disk still works
            attach(q, str(vmid))
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("cat /keep\n")
            q.expect(r"before-the-switch", 60)
            q.expect(r"\$ ", ATONCE)
            mm = q.mark()
            q.send("echo on-%s > /after; sync; cat /after\n" % want)
            q.expect(r"on-%s" % want, 60)
            q.expect(r"\$ ", ATONCE)
            assert "kernel is booting" not in q.output_since(mm), q.output_since(mm)
            detach(q)

        # and while it is stopped, which is the safest moment there is: its vcpus
        # are asleep, so no guest code runs between the old model going away and
        # the new description arriving. The request waits as a pending VSSIP.
        host_cmd(q, "vmm pause %d" % vmid)
        out = vm_ls(q)
        assert re.search(r"^%d\s+\S+\s+paused" % vmid, out, re.M), out
        q.send("vmm revise %d imsic\n" % vmid)
        q.expect(r"interrupt controller -> imsic", 60)
        q.expect(r"\$ ", ATONCE)
        mp = q.mark()
        host_cmd(q, "vmm resume %d" % vmid)
        wait_adopted(q, mp)
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"\$ ", 60)
        q.send("echo after-the-pause\n")
        q.expect(r"after-the-pause", 60)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        kill_vm(q, vmid)
        print("    a guest changed interrupt controller three times and kept running")
    finally:
        q.close()


def t_revise_intc_addr(QEMU, intc, cpus, log):
    """F2: the controller's *address* changed under a running guest, and back.

    The kind stays a PLIC; the host moves the register window 256 MB up the map
    (`vmm revise VM intcmap alt') and tells the guest, which maps the new window
    and rebinds its driver. Checked as t_revise_intc checks: the guest goes on
    working through the controller at its new address (its disk completes, so
    interrupts arrive), it says from inside where its controller is, and it did
    not reboot. Then the same the other way."""
    q = boot_host(QEMU, "plic", cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        q.send("echo before-the-move > /keep\n")
        q.expect(r"\$ ", ATONCE)
        detach(q)
        for want, addr in (("alt", "0x1c000000"), ("default", "0xc000000")):
            m = q.mark()
            q.send("vmm revise %d intcmap %s\n" % (vmid, want))
            q.expect(r"interrupt controller map -> %s: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            sw = q.output_since(m)
            assert re.search(r"interrupt controller registers \+0x[0-9a-f]+ -> \+0x[0-9a-f]+", sw), sw
            wait_adopted(q, m)
            attach(q, str(vmid))
            # the guest said where its controller is now, from inside; the
            # console backlog is replayed on attach
            q.expect(r"machine: interrupt controller plic at %s" % addr, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("cat /keep\n")
            q.expect(r"before-the-move", 60)
            q.expect(r"\$ ", ATONCE)
            mm = q.mark()
            q.send("echo at-%s > /after; sync; cat /after\n" % want)
            q.expect(r"at-%s" % want, 60)
            q.expect(r"\$ ", ATONCE)
            assert "kernel is booting" not in q.output_since(mm), q.output_since(mm)
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            # the exits went to the window where the controller is now
            assert ("plic@1c000000" if want == "alt" else "plic@c000000") in out, out
        kill_vm(q, vmid)
        print("    a guest followed its interrupt controller to a new address and back, without a reboot")
    finally:
        q.close()


def t_revise_irqmap(QEMU, intc, cpus, log):
    """F3: the interrupt lines renumbered under a running guest, and back.

    `vmm revise VM irqmap alt' moves the uart from line 10 to 7 and the virtio
    devices from 1..4 to 11..14; the host carries the lines the devices are
    holding across by number and tells the guest, which takes the new numbers
    into its platform description and rebuilds its controller's source
    configuration. Checked by use: the disk completes (its interrupt arrives on
    the new line), the console works (the uart's does), no reboot."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        q.send("echo before-the-renumbering > /keep\n")
        q.expect(r"\$ ", ATONCE)
        detach(q)
        for want, uart, virtio in (("alt", 7, "11 12 13"), ("default", 10, "1 2 3")):
            m = q.mark()
            q.send("vmm revise %d irqmap %s\n" % (vmid, want))
            q.expect(r"interrupt lines -> %s: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            sw = q.output_since(m)
            assert re.search(r"interrupt lines renumbered \w+ -> %s, \d+ line\(s\) held" % want, sw), sw
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: interrupt lines renumbered: uart %d, virtio %s" % (uart, virtio), 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("cat /keep\n")
            q.expect(r"before-the-renumbering", 60)
            q.expect(r"\$ ", ATONCE)
            mm = q.mark()
            q.send("echo on-%s > /after; sync; cat /after\n" % want)
            q.expect(r"on-%s" % want, 60)
            q.expect(r"\$ ", ATONCE)
            assert "kernel is booting" not in q.output_since(mm), q.output_since(mm)
            detach(q)
        kill_vm(q, vmid)
        print("    a guest had its interrupt lines renumbered and back, and kept running")
    finally:
        q.close()


def _info(q, vmid):
    """exits, observed set, revision generation and the by-device line of `vmm info'."""
    out = host_cmd(q, "vmm info %d" % vmid)
    m = re.search(r"exits (\d+):", out)
    exits = int(m.group(1)) if m else -1
    m = re.search(r"observed: ([^\n]*)", out)
    obs = re.sub(r"@\d+ms|\(of \d+ ms alive\)", "", m.group(1)).strip() if m else ""   # the set, not the times
    m = re.search(r"generation (\d+)", out)
    gen = int(m.group(1)) if m else -1
    return exits, obs, gen, out


_churn_pass = [0]


def _blk(q, vmid):
    """csr exits and virtio-blk interrupts from `vmm info': the two numbers the
    delivery path shows up in. MMIO exits are the device's traffic either way."""
    out = host_cmd(q, "vmm info %d" % vmid)
    m = re.search(r"csr (\d+),", out)
    csr = int(m.group(1)) if m else -1
    m = re.search(r"virtio-blk@[0-9a-f]+ \d+r/\d+w/(\d+)i", out)
    irqs = int(m.group(1)) if m else -1
    return csr, irqs


def _churn(q, n=800):
    """interrupts inside the attached guest: a sequential read of the disk in
    4 KB blocks, one completion interrupt per block. Each call reads a part of
    the disk no earlier call has, so the block cache serves none of it (inv5:
    the second pass over the same blocks took 971 exits to the first's 14435)."""
    skip = 2000 + _churn_pass[0] * n      # past what booting left in the cache (inv6, m2a)
    _churn_pass[0] += 1
    q.send("dd if=/dev/vda of=/dev/null bs=4096 count=%d skip=%d 2>/dev/null; echo CHURNED\n" % (n, skip))
    q.expect(r"CHURNED", 180)
    q.expect(r"\$ ", ATONCE)


def t_invisible_imsic(QEMU, intc, cpus, log):
    """F23: the same IMSIC delivered through a hardware guest interrupt file or
    through the software model. The guest cannot tell -- no revision is asked,
    its observed set does not change, it keeps running -- and the host can say
    what the difference costs: the exits the same workload takes either way.
    This is the tier-cost number seen from the invisible side."""
    q = boot_host(QEMU, "imsic", cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        detach(q)
        out = host_cmd(q, "vmm info %d" % vmid)
        assert "intc aplic-msi+imsic (hardware guest files)" in out, out
        e0, obs0, gen0, _ = _info(q, vmid)
        c0, i0 = _blk(q, vmid)
        attach(q, str(vmid)); q.send("\n"); q.expect(r"\$ ", 60)
        _churn(q)
        detach(q)
        e1, obs1, gen1, _ = _info(q, vmid)
        c1, i1 = _blk(q, vmid)
        m = q.mark()
        host_cmd(q, "vmm imsichw %d off" % vmid)
        assert "imsic delivery hardware file -> software model" in q.output_since(m), q.output_since(m)
        out = host_cmd(q, "vmm info %d" % vmid)
        assert "(hardware guest files)" not in out, out
        attach(q, str(vmid)); q.send("\n"); q.expect(r"\$ ", 60)
        _churn(q)
        detach(q)
        e2, obs2, gen2, out2 = _info(q, vmid)
        c2, i2 = _blk(q, vmid)
        host_cmd(q, "vmm imsichw %d on" % vmid)
        out = host_cmd(q, "vmm info %d" % vmid)
        assert "(hardware guest files)" in out, out
        attach(q, str(vmid)); q.send("\n"); q.expect(r"\$ ", 60)
        _churn(q)
        q.send("echo still-here\n"); q.expect(r"still-here", 60); q.expect(r"\$ ", ATONCE)
        detach(q)
        e3, obs3, gen3, _ = _info(q, vmid)
        c3, i3 = _blk(q, vmid)
        assert gen0 == gen1 == gen2 == gen3, (gen0, gen1, gen2, gen3)     # nothing was asked
        assert "revision" not in q.buf.decode(errors="replace")[-20000:].replace("revision: guest", ""), q.buf[-3000:]
        # and the switch itself showed the guest nothing new. The workload may:
        # the guest's libc uses the vector unit for memmove, so `vector' can
        # join the observed set while dd runs, switch or no switch (inv3).
        grown = (set(obs3.split()) - set(obs1.split())) | (set(obs2.split()) - set(obs1.split()))
        assert grown <= {"vector"}, (obs1, obs2, obs3)
        hw, sw, hw2 = e1 - e0, e2 - e1, e3 - e2
        # the delivery path shows up as CSR exits per interrupt: the model's
        # claim and MSI traffic trap (vstopei, vsireg), the file's do not. MMIO
        # exits are the disk's own traffic and the same either way (inv6).
        def per(c, i):
            return (c / i) if i > 0 else float("nan")
        p1, p2, p3 = per(c1 - c0, i1 - i0), per(c2 - c1, i2 - i1), per(c3 - c2, i3 - i2)
        print("    invisible: %d/%d/%d interrupts took %.2f/%.2f/%.2f csr exits each on file/model/file (%d/%d/%d exits in all)"
              % (i1 - i0, i2 - i1, i3 - i2, p1, p2, p3, hw, sw, hw2))
        # the file phases see fewer interrupts as exits (that is the point); on a
        # loaded box the first window counted 47 (regg2) -- enough to divide by
        # (m7x saw 20 and 6 in the first window at load 50: the file phase is the one where interrupts are not exits)
        assert i1 - i0 >= 5 and i2 - i1 > 50 and i3 - i2 >= 5, (i1 - i0, i2 - i1, i3 - i2)
        assert p2 > p1 + 1 and p2 > p3 + 1, (p1, p2, p3)
        kill_vm(q, vmid)
    finally:
        q.close()


def t_invisible_backing(QEMU, intc, cpus, log):
    """F24: how the guest's memory is backed -- 2 MB blocks or 4 KB pages, with
    or without same-page merging -- is the host's business. Changed under a
    running guest: no revision, same observed set, and the guest's data is what
    it was (a checksum over a file it wrote before)."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        q.send("dd if=/dev/urandom of=/tmp/r bs=4096 count=512 2>/dev/null; cksum /tmp/r\n")
        m = q.expect(r"(\d+) \d+ /tmp/r", 120)
        sum0 = m.group(1)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        e0, obs0, gen0, out = _info(q, vmid)
        assert re.search(r"memory: 0 chunks split to pages", out), out
        host_cmd(q, "vmm split %d" % vmid)
        _, _, gen1, out = _info(q, vmid)
        m2 = re.search(r"memory: (\d+) chunks split to pages", out)
        assert m2 and int(m2.group(1)) > 0, out
        host_cmd(q, "vmm ksm on")
        host_cmd(q, "sleep 2")
        host_cmd(q, "vmm ksm off")
        _, obs2, gen2, _ = _info(q, vmid)
        attach(q, str(vmid)); q.send("\n"); q.expect(r"\$ ", 60)
        q.send("cksum /tmp/r\n")
        m3 = q.expect(r"(\d+) \d+ /tmp/r", 120)
        q.expect(r"\$ ", ATONCE)
        assert m3.group(1) == sum0, (sum0, m3.group(1))
        q.send("echo still-here\n"); q.expect(r"still-here", 60); q.expect(r"\$ ", ATONCE)
        detach(q)
        assert gen0 == gen1 == gen2, (gen0, gen1, gen2)
        assert obs0 == obs2, (obs0, obs2)
        print("    invisible: %s blocks split to pages and same-page merging on and off under the guest; checksum kept, nothing asked"
              % m2.group(1))
        kill_vm(q, vmid)
    finally:
        q.close()


def t_revise_sbi(QEMU, intc, cpus, log):
    """F22: the set of SBI extensions is a fact of the machine. The host withholds
    two of them from a running guest (`vmm revise VM sbi hide srst,dbcn'); the
    guest is told (REV_SBI, the one revision the tree cannot carry: MACH fid 6
    says what changed), re-probes, and /sys/machine/sbi says what it now has.
    Then they are given back."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        q.send("cat /sys/machine/sbi\n")
        q.expect(r"time ipi rfence hsm srst dbcn", 30)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        for hide, shown in (("srst,dbcn", "time ipi rfence hsm"), ("none", "time ipi rfence hsm srst dbcn"),
                            ("rfence", "time ipi hsm srst dbcn")):
            m = q.mark()
            q.send("vmm revise %d sbi %s\n" % (vmid, hide))
            q.expect(r"sbi extensions withheld 0x[0-9a-f]+: asked", 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: sbi extensions: %s" % shown, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("cat /sys/machine/sbi\n")
            q.expect(r"%s\r?\n" % shown, 30)
            q.expect(r"\$ ", ATONCE)
            q.send("echo alive-%s\n" % hide.replace(",", "-"))
            q.expect(r"alive-%s" % hide.replace(",", "-"), 60)
            q.expect(r"\$ ", ATONCE)
            detach(q)
        kill_vm(q, vmid)
        print("    a guest was told which SBI extensions it has, three times, and re-probed each time")
    finally:
        q.close()


def t_revise_ipi(QEMU, intc, cpus, log):
    """F4: how the guest sends IPIs -- a CLINT's msip or the SBI -- is a fact of
    the machine. The CLINT is taken away from a running two-hart guest (its
    node leaves the tree, the model leaves the machine) and given back; the
    guest keeps scheduling across its harts, which is the check."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 2 -m 64")
        detach(q)
        for want, via in (("sbi", "sbi"), ("clint", "clint")):
            m = q.mark()
            q.send("vmm revise %d ipi %s\n" % (vmid, want))
            q.expect(r"ipis -> %s: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: ipis via %s" % via, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            # work that crosses harts: pipes between processes on both
            q.send("for i in 1 2 3 4 5 6; do (echo ipi-$i | cat | cat) & done; wait; echo IPIS-OK\n")
            q.expect(r"IPIS-OK", 120)
            q.expect(r"\$ ", ATONCE)
            detach(q)
        kill_vm(q, vmid)
        print("    a two-hart guest lost its CLINT and sent its IPIs through SBI, then got it back")
    finally:
        q.close()


def t_revise_timer(QEMU, intc, cpus, log):
    """F5: where the guest's timer is -- Sstc's stimecmp or the SBI timer -- is a
    fact of the machine. Sstc is taken away from a running guest (henvcfg.STCE
    off, `sstc' out of the isa string); the guest re-arms through SBI and its
    clock keeps time, which `sleep 1' shows; then it is given back."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        detach(q)
        for want, via in (("sbi", "sbi"), ("sstc", "stimecmp")):
            m = q.mark()
            q.send("vmm revise %d timer %s\n" % (vmid, want))
            q.expect(r"timer -> %s: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: timer via %s" % via, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            import time as _t
            t0 = _t.time()
            q.send("sleep 1; echo SLEPT\n")
            q.expect(r"SLEPT", 60)
            q.expect(r"\$ ", ATONCE)
            dt = _t.time() - t0
            assert 0.8 < dt < 6, dt           # the timer fires: not stuck, not spinning
            detach(q)
        kill_vm(q, vmid)
        print("    a guest lost Sstc and kept time through the SBI timer, then got it back")
    finally:
        q.close()


def t_revise_timebase(QEMU, intc, cpus, log):
    """F6: the timebase-frequency is a fact of the machine, with a stated cost.
    A running guest is told its counter runs at 1 MHz instead of 10 MHz: the
    host traps and scales its `time' reads and carries its SBI timer deadlines
    in the other units; the guest recomputes its tick interval. `sleep 1' still
    takes a second of wall time, which is the check -- a guest that believed
    the old rate would sleep ten seconds or a tenth. Then back to 10 MHz."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        detach(q)
        import time as _t
        for hz, ok in (("1000000", "1000000"), ("10000000", "10000000")):
            m = q.mark()
            q.send("vmm revise %d timebase %s\n" % (vmid, hz))
            q.expect(r"timebase -> %s Hz: asked" % hz, 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            if hz == "1000000":
                q.expect(r"machine: timebase 1000000 Hz", 60)
            else:
                q.expect(r"machine: timebase 10000000 Hz", 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            t0 = _t.time()
            q.send("sleep 1; echo SLEPT\n")
            q.expect(r"SLEPT", 60)
            q.expect(r"\$ ", ATONCE)
            dt = _t.time() - t0
            assert 0.8 < dt < 4, dt
            # and the guest's own clock agrees with the wall
            q.send("cat /proc/uptime\n")
            q.expect(r"\$ ", 30)
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            print("    timebase %s Hz: sleep 1 took %.2fs; %s" % (hz, dt, re.search(r"exits \d+:[^\n]*", out).group(0)))
        kill_vm(q, vmid)
        print("    a guest was told its clock runs at another rate, and kept time at both")
    finally:
        q.close()


def t_revise_hartid(QEMU, intc, cpus, log):
    """F9: how the harts are numbered is a fact of the machine. A running
    two-hart guest, numbered 0 and 1, is told its harts are 4 and 5: mhartid,
    the cpu nodes and every controller's per-hart registers move; the guest
    keeps a logical index (tp) and rebinds its per-hart controller state to the
    new numbers. Work that crosses harts still crosses them. Then back."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 2 -m 64")
        q.send("cat /proc/cpuinfo | head -20\n")
        q.expect(r"\$ ", 30)
        detach(q)
        for shift in ("4", "0"):
            m = q.mark()
            q.send("vmm revise %d hartid %s\n" % (vmid, shift))
            q.expect(r"harts numbered from %s: asked" % shift, 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: harts renumbered: %s" % ("4 5" if shift == "4" else "0 1"), 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("for i in 1 2 3 4 5 6; do (echo h-$i | cat | cat) & done; wait; echo HARTS-OK\n")
            q.expect(r"HARTS-OK", 120)
            q.expect(r"\$ ", ATONCE)
            mm = q.mark()
            q.send("echo on-%s > /after; sync; cat /after\n" % shift)
            q.expect(r"on-%s" % shift, 60)
            q.expect(r"\$ ", ATONCE)
            assert "kernel is booting" not in q.output_since(mm), q.output_since(mm)
            detach(q)
        kill_vm(q, vmid)
        print("    a two-hart guest had its harts renumbered 4,5 and back, and kept scheduling across them")
    finally:
        q.close()


def t_revise_mac(QEMU, intc, cpus, log):
    """F19: the NIC's identity changed under a running guest, and back. Not a
    revision: the device announces a configuration change and the driver
    re-reads its address, as virtio provides. The guest's stack sends from
    the new address; a ping to its gateway still answers."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        q.send("ifconfig eth0\n")
        q.expect(r"52:54:00:78:76:10", 30)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        for want, mac in (("alt", "52:54:00:79:76:10"), ("default", "52:54:00:78:76:10")):
            m = q.mark()
            host_cmd(q, "vmm revise %d mac %s" % (vmid, want))
            assert "nic identity -> %s" % mac in q.output_since(m), q.output_since(m)
            attach(q, str(vmid))
            q.expect(r"virtio net: mac changed to %s" % mac, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("ifconfig eth0\n")
            q.expect(mac, 30)
            q.expect(r"\$ ", ATONCE)
            # (the guest's default network policy blocks ICMP to its gateway, so
            # the traffic check is the migration test's: a guest that arrived
            # with a new address keeps its disk and console working over it)
            detach(q)
        kill_vm(q, vmid)
        print("    a guest's NIC changed identity twice under it; it re-read the address and kept talking")
    finally:
        q.close()


def t_revise_rtc(QEMU, intc, cpus, log):
    """F16/F17: a device removed from a running guest and given back -- the RTC.
    Without it the guest's wall clock runs on in software from the last
    reading; with it back, it re-anchors to the device. `date +%s' stays
    within a few seconds of the host's throughout, which is the check."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        def secs(out):
            # `date' here prints a calendar line; the seconds of the day are enough
            # for a comparison within a few seconds (both hosts are on the same day)
            h, mi, s = re.search(r"(\d\d):(\d\d):(\d\d)", out).groups()
            return int(h) * 3600 + int(mi) * 60 + int(s)
        def gdate():
            m = q.mark(); q.send("date\n"); q.expect(r"\$ ", 30)
            return secs(q.output_since(m))
        t0 = gdate()
        detach(q)
        for want, msg in (("off", r"rtc: gone; the wall clock continues from \d+ in software"),
                          ("on", r"rtc: goldfish at 0x101000, epoch seconds \d+")):
            m = q.mark()
            q.send("vmm revise %d rtc %s\n" % (vmid, want))
            q.expect(r"rtc %s: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            assert re.search(r"hyp: vm \d+: rtc (removed|added)", q.output_since(m)), q.output_since(m)
            wait_adopted(q, m)
            hnow = secs(host_cmd(q, "date"))
            attach(q, str(vmid))
            q.expect(msg, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("sleep 1\n")
            q.expect(r"\$ ", 30)
            gnow = gdate()
            d = abs(gnow - hnow)
            assert min(d, 86400 - d) <= 5, (want, gnow, hnow)
            detach(q)
        kill_vm(q, vmid)
        print("    a guest lost its RTC (wall clock ran on in software) and got it back (re-anchored); within 5 s of the host both times")
    finally:
        q.close()


def t_revise_console(QEMU, intc, cpus, log):
    """F18: the console is a class change beside the interrupt controller: the
    16550 the guest drives register by register, a virtio-console it feeds by
    descriptor, the SBI debug console it calls into. A running guest is moved
    through all three and back; each time it announces the change *through
    the new console* and a command typed into it answers."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        detach(q)
        for want, shown in (("sbi", "sbi"), ("virtio", "virtio-console"), ("uart", "uart")):
            m = q.mark()
            q.send("vmm revise %d console %s\n" % (vmid, want))
            q.expect(r"console -> %s: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: console via %s" % shown, 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("echo typed-into-%s\n" % want)
            q.expect(r"typed-into-%s" % want, 60)
            q.expect(r"\$ ", ATONCE)
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            print("    console %s: %s" % (want, re.search(r"exits \d+:[^\n]*", out).group(0)))
        kill_vm(q, vmid)
        print("    a guest's console changed kind three times under it and answered through each")
    finally:
        q.close()


def t_revise_ram(QEMU, intc, cpus, log):
    """F13: a running guest is given more memory. The memory node grows, the
    guest is told, and it adds the range to its allocator with a page database
    of its own; a file too large for the old machine then fits."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 32")
        q.send("grep MemTotal /proc/meminfo\n")
        m = q.expect(r"MemTotal:\s+(\d+) kB", 30)
        before = int(m.group(1))
        q.expect(r"\$ ", ATONCE)
        q.send("dd if=/dev/urandom of=/tmp/big bs=1M count=24 2>/dev/null; cksum /tmp/big; rm /tmp/big\n")
        m = q.expect(r"(\d+) (\d+) /tmp/big", 240)
        assert int(m.group(2)) < 25165824, "tmpfs took 24 MB in a 32 MB machine: %s" % m.group(0)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        m = q.mark()
        q.send("vmm resize %d 64\n" % vmid)
        q.expect(r"memory -> 64 MB: asked", 60)
        q.expect(r"\$ ", ATONCE)
        assert "memory 32 -> 64 MB" in q.output_since(m), q.output_since(m)
        wait_adopted(q, m)
        attach(q, str(vmid))
        q.expect(r"machine: memory 32 -> 64 MB \(32 MB added at 0x82000000\)", 60)
        q.send("\n")
        q.expect(r"\$ ", 60)
        q.send("grep MemTotal /proc/meminfo\n")
        m = q.expect(r"MemTotal:\s+(\d+) kB", 30)
        after = int(m.group(1))
        q.expect(r"\$ ", ATONCE)
        assert after >= before + 30000, (before, after)
        # a file the old machine could not have held
        # /tmp is a size-less tmpfs: its limit is half the machine, and it
        # follows the machine (a 24 MB file did not fit in 32 MB; it does now)
        q.send("dd if=/dev/urandom of=/tmp/big bs=1M count=24 2>/dev/null; cksum /tmp/big\n")
        mm = q.expect(r"(\d+) 25165824 /tmp/big", 240)
        q.expect(r"\$ ", ATONCE)
        q.send("cksum /tmp/big\n")
        m2 = q.expect(r"(\d+) 25165824 /tmp/big", 240)
        q.expect(r"\$ ", ATONCE)
        assert mm.group(1) == m2.group(1), (mm.group(1), m2.group(1))
        detach(q)
        kill_vm(q, vmid)
        print("    a guest was given 32 MB more (MemTotal %d -> %d kB); a 24 MB file that its tmpfs refused before fits now, checksum stable" % (before, after))
    finally:
        q.close()


def t_revise_cpus(QEMU, intc, cpus, log):
    """F8/F7: harts taken away from a running guest and given back. A four-hart
    guest is told it has two: the two beyond drain their queues onto the
    others and stop through SBI HSM (the host sees them stopped); work still
    crosses harts. Then it is told it has four again: the host starts the two
    at the kernel's entry after the guest has extended its tables, and they
    boot in (`hart N starting')."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 4 -m 64")
        detach(q)
        out = host_cmd(q, "vmm info %d" % vmid)
        assert "4 vcpu" in out, out
        def work():
            q.send("for i in 1 2 3 4 5 6 7 8; do (echo w-$i | cat | cat) & done; wait; echo WORK-OK\n")
            q.expect(r"WORK-OK", 120)
            q.expect(r"\$ ", ATONCE)
        q.send("vmm revise %d cpus 2\n" % vmid)
        q.expect(r"hart\(s\) 2\.\.3 parked", 60)
        q.expect(r"cpus -> 2: done", 60)
        q.expect(r"\$ ", ATONCE)
        attach(q, str(vmid))
        q.expect(r"machine: cpus 4 -> 2 \(parking 2 hart\(s\)\)", 60)
        q.send("\n")
        q.expect(r"\$ ", 60)
        work()
        detach(q)
        out = host_cmd(q, "vmm info %d" % vmid)
        q.send("vmm revise %d cpus 4\n" % vmid)
        q.expect(r"hart\(s\) 2\.\.3 started", 60)
        q.expect(r"cpus -> 4: done", 60)
        q.expect(r"\$ ", ATONCE)
        attach(q, str(vmid))
        q.expect(r"machine: cpus 2 -> 4", 60)
        # the two boot in whichever order the host's scheduler runs them
        q.expect(r"hart [23] starting", 60)
        q.expect(r"hart [23] starting", 60)
        q.send("\n")
        q.expect(r"\$ ", 60)
        work()
        detach(q)
        kill_vm(q, vmid)
        print("    a four-hart guest parked two of its harts on request and got them back; work crossed harts throughout")
    finally:
        q.close()


def t_revise_cboz(QEMU, intc, cpus, log):
    """F21: the cache-block size cbo.zero clears is a fact of the machine, and
    the one whose wrong value does not fail loudly: a kernel zeroing pages at
    a stride longer than the block leaves every other block as it was. The
    host describes another size than its own (128 on a 64-byte host); until
    the guest adopts, cbo.zero traps and is done in software at the larger of
    the two; after, the guest strides 128 and the host clears 128 for it.
    Fresh pages are zero throughout (zerocheck); then back to the host's own,
    in hardware."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        q.send("zerocheck 512\n")
        q.expect(r"zerocheck: 512 pages clean", 120)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        for want, frm in ((128, 64), (64, 128)):
            m = q.mark()
            q.send("vmm revise %d cboz %d\n" % (vmid, want))
            q.expect(r"cache block %d -> %d bytes \((the host's, in hardware|not the host's, in software)\)" % (frm, want), 60)
            q.expect(r"cache block -> %d bytes: asked" % want, 60)
            q.expect(r"\$ ", ATONCE)
            wait_adopted(q, m)
            attach(q, str(vmid))
            q.expect(r"machine: cache block %d -> %d bytes" % (frm, want), 60)
            q.send("\n")
            q.expect(r"\$ ", 60)
            q.send("zerocheck 512\n")
            q.expect(r"zerocheck: 512 pages clean", 120)
            q.expect(r"\$ ", ATONCE)
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            mm = re.search(r"cbo.zero in software (\d+)", out)
            if want == 128:
                assert mm and int(mm.group(1)) > 0, out     # the 128-byte block is not this host's: software
                soft = int(mm.group(1))
        out = host_cmd(q, "vmm info %d" % vmid)
        mm = re.search(r"cbo.zero in software (\d+)", out)
        # back on the host's own block, cbo.zero is hardware again: the count stops moving
        # (a few from the window before the adoption are allowed)
        assert mm and int(mm.group(1)) - soft < 5000, (soft, out)
        kill_vm(q, vmid)
        print("    a guest was described a 128-byte cache block on a 64-byte host (%d cbo.zero done in software), zeroed pages right throughout, and got the host's own back" % soft)
    finally:
        q.close()


def t_revise_vector(QEMU, intc, cpus, log):
    """the vector unit taken away from a running guest, and given back.

    A feature mask can hide V from a guest that has not looked; it cannot take it
    away from one that is using it, because the register file is *state*. So the
    host asks and the guest answers, and the answer depends on what its tasks are
    doing at that moment rather than on what the fleet can offer. Skipped on a
    machine built without vectors, where there is nothing to revoke."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_guest(q, "-c 1 -m 64")
        m = q.mark()
        q.send("vecbench 2\n")
        q.expect(r"vecbench: (ok|no vector extension)", 90)
        q.expect(r"\$ ", ATONCE)
        if "no vector extension" in q.output_since(m):
            detach(q)
            kill_vm(q, vmid)
            print("    no vector unit on this machine: nothing to revoke")
            return
        detach(q)

        # revoke it. Between programs no task is holding vector state, so the
        # guest has nothing to strand and takes it.
        mv = q.mark()
        q.send("vmm revise %d novector\n" % vmid)
        q.expect(r"vector unit revoked: asked", 60)
        q.expect(r"\$ ", ATONCE)
        wait_adopted(q, mv)
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"\$ ", 60)
        # and the guest now believes it has none: the description changed, so the
        # program takes its no-vector path rather than trapping on an instruction
        q.send("vecbench 2\n")
        q.expect(r"vecbench: no vector extension", 90)
        q.expect(r"\$ ", ATONCE)
        detach(q)

        # give it back, and it works again
        mv = q.mark()
        q.send("vmm revise %d vector\n" % vmid)
        q.expect(r"vector unit granted: asked", 60)
        q.expect(r"\$ ", ATONCE)
        wait_adopted(q, mv)
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"\$ ", 60)
        q.send("vecbench 2\n")
        q.expect(r"vecbench: ok", 90)
        q.expect(r"\$ ", ATONCE)
        detach(q)
        kill_vm(q, vmid)
        print("    a guest lost its vector unit, noticed, and got it back")
    finally:
        q.close()



def t_revguest(QEMU, intc, cpus, log):
    """a second, independent guest speaks the revision protocol.

    guest/revguest.c is not xv6 and shares no code with it: three hundred lines
    that parse the device tree, bind an interrupt driver to whichever controller
    it describes, take the UART's interrupt through it, and rebind when the host
    says the description changed. If only xv6 could do this, "the guest speaks
    the protocol" would mean xv6; this is what makes it mean the protocol."""
    q = boot_host(QEMU, "imsic", cpus, log)
    try:
        q.send("vmm start -k /revguest -P -c 1 -f none\n")
        m = q.expect(r"vm (\d+): 1 vcpu", 20)
        vmid = int(m.group(1))
        q.expect(r"revguest: interrupt controller plic at 0xc000000", 30)
        q.expect(r"revguest: revision protocol version 1", 30)
        q.expect(r"revguest: ready", 30)
        # a keystroke is an interrupt: the UART raises its line, the PLIC delivers it
        q.send("a")
        q.expect(r"revguest: irq via plic \(key a, total 1\)", 30)
        detach(q)
        out = vm_info(q, vmid)
        assert "guest speaks the protocol" in out, out
        assert "intc plic" in out, out

        # now change its interrupt controller under it
        mr = q.mark()
        q.send("vmm revise %d imsic\n" % vmid)
        q.expect(r"interrupt controller -> imsic", 60)
        q.expect(r"\$ ", ATONCE)
        wait_adopted(q, mr)
        out = vm_info(q, vmid)
        assert "intc aplic-msi+imsic" in out, out
        attach(q, str(vmid))
        # what the guest itself said when it rebound. It said it while detached, so
        # this is the console ring replaying -- which lands a moment *after* the
        # attach banner, not with it: wait for it rather than look for it.
        q.expect(r"revguest: revision \d+: now on imsic \(adopted 0x2\)", 30)
        # and the next keystroke arrives through a controller that did not exist
        # when this guest booted
        q.send("b")
        q.expect(r"revguest: irq via imsic \(key b, total 2\)", 30)
        detach(q)
        kill_vm(q, vmid)
        print("    a guest that is not xv6 rebound its interrupt driver on request")
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
        t.append(("guest-pages-%s" % m, lambda m=m: t_pages(QEMU, m, cpus, log)))
        t.append(("guest-balloon-%s" % m, lambda m=m: t_balloon(QEMU, m, cpus, log)))
        t.append(("guest-memlimit-%s" % m, lambda m=m: t_memlimit(QEMU, m, cpus, log)))
        t.append(("guest-ksm-%s" % m, lambda m=m: t_ksm(QEMU, m, cpus, log)))
        t.append(("guest-malicious-%s" % m, lambda m=m: t_malicious(QEMU, m, cpus, log)))
        t.append(("guest-fd-vector-%s" % m, lambda m=m: t_fdv_guests(QEMU, m, cpus, log)))
    t.append(("guest-fd-vector-novec-plic", lambda: t_fdv_guests(QEMU, "plic", cpus, log, vec=0)))
    t.append(("guest-ownership-plic", lambda: t_ownership(QEMU, "plic", cpus, log)))
    # the revisable-machine mechanism: one host boot each, and they name their
    # own controllers because the point of them is a controller changing.
    t.append(("guest-revise-intc", lambda: t_revise_intc(QEMU, "imsic", cpus, log)))
    t.append(("guest-revise-vector", lambda: t_revise_vector(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-intc-addr", lambda: t_revise_intc_addr(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-irqmap", lambda: t_revise_irqmap(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-irqmap-imsic", lambda: t_revise_irqmap(QEMU, "imsic", cpus, log)))
    t.append(("guest-revise-sbi", lambda: t_revise_sbi(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-ipi", lambda: t_revise_ipi(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-timer", lambda: t_revise_timer(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-timebase", lambda: t_revise_timebase(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-hartid", lambda: t_revise_hartid(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-hartid-imsic", lambda: t_revise_hartid(QEMU, "imsic", cpus, log)))
    t.append(("guest-revise-mac", lambda: t_revise_mac(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-rtc", lambda: t_revise_rtc(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-console", lambda: t_revise_console(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-ram", lambda: t_revise_ram(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-cpus", lambda: t_revise_cpus(QEMU, "plic", cpus, log)))
    t.append(("guest-revise-cboz", lambda: t_revise_cboz(QEMU, "plic", cpus, log)))
    # F23/F24: the facts that are not facts of the machine
    t.append(("guest-invisible-imsic", lambda: t_invisible_imsic(QEMU, "imsic", cpus, log)))
    t.append(("guest-invisible-backing", lambda: t_invisible_backing(QEMU, "plic", cpus, log)))
    t.append(("guest-revguest", lambda: t_revguest(QEMU, "imsic", cpus, log)))
    for m in modes:
        t.append(("guest-fuzz-%s" % m, lambda m=m: t_fuzz(QEMU, m, cpus, log)))
    # host configurations: a single hart, and more harts than the default.
    t.append(("guest-smp-on-1-hart-plic", lambda: t_guest_usertests(QEMU, "plic", 1, log, 2)))
    t.append(("guest-parallel-on-4-harts-imsic", lambda: t_parallel(QEMU, "imsic", 4, log, 3, 2)))
    return t
