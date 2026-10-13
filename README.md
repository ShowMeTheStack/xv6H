# xv6H

xv6H is a Type-1 hypervisor for RISC-V machines with the hypervisor (H)
extension. It started as MIT's xv6-riscv teaching kernel and adds a
hypervisor whose guests can be told, while they run, that their machine has
changed. A running guest's description (its interrupt controller, timer,
timebase, harts, vector unit, memory, device map and the rest) can be
revised in place or across a live migration to a host of another shape, and
the guest adopts the new description without a reboot.

The repository holds the hypervisor and the operating system around it, its
own guests, a Linux module and kernel patches that let Linux take part,
patches to KVM and kvmtool that implement the same protocol a second time, a
port to an FPGA board with a Rocket core that has the H extension, Spin
models of the protocol, the test harness, and every script used to produce
the measurements in the paper cited at the end.

## Contents

1. [What xv6H is](#what-xv6h-is)
2. [RISC-V virtualisation with the H extension](#risc-v-virtualisation-with-the-h-extension)
3. [Revisable machine descriptions](#revisable-machine-descriptions)
4. [Repository layout](#repository-layout)
5. [Building](#building)
6. [Running](#running)
7. [Linux guests](#linux-guests)
8. [KVM and kvmtool](#kvm-and-kvmtool)
9. [FPGA: Rocket with the H extension on a ZedBoard](#fpga-rocket-with-the-h-extension-on-a-zedboard)
10. [Model checking](#model-checking)
11. [Tests](#tests)
12. [Reproducing the paper's results](#reproducing-the-papers-results)
13. [Licence](#licence)
14. [Citation](#citation)

## What xv6H is

[xv6](https://github.com/mit-pdos/xv6-riscv) is a re-implementation of Unix
Version 6 for RISC-V, written at MIT by Frans Kaashoek, Robert Morris and
Russ Cox for the operating systems course 6.1810, with contributions from
many others (the original acknowledgements are in [`user/README`](user/README), which also becomes `/README` in the file system image). xv6H
branches from xv6-riscv at commit `9e3161a`. It keeps xv6's structure: the
trap path, the page-table walker, the file-descriptor layer, `fork` and
`exec`, and the coding style. Everything else was written for this tree; no
code was taken from another kernel.

The same `kernel/kernel` binary runs as the host, in HS mode and owning M
mode, and as a guest in VS mode. A host runs several guests at once, each
with several vCPUs, on QEMU's `virt` machine with any of its three interrupt
controller configurations (PLIC, APLIC, APLIC-MSI with IMSIC), and on a
Rocket core on an FPGA. Guests get a 16550 console, virtio-mmio block,
network, balloon and console devices, a real-time clock, timers and IPIs.
Guest memory is backed on demand, can be ballooned and merged between
machines, and machines can be saved, snapshotted, cloned and migrated live
to another host over the host's own TCP stack.

Around the hypervisor, xv6H grew into a multi-user system: ext4 with a jbd2
journal that Linux can replay, a network stack (virtio-net, ARP, IPv4, ICMP,
UDP, DHCP, TCP, sockets) with a software switch for guests, TLS through
BearSSL, a weighted fair scheduler, POSIX signals and terminals, Sv48 paging
up to 512 GiB of RAM, namespaces, control groups, a system-call filter and
containers, a dynamic linker, a shell, and about two hundred utilities each
with a manual page.

The part that motivates the paper is the *revisable machine description*.
Hypervisors normally fix a guest's description at boot, which is why fleets
mask CPU features to a common floor and pin guests to hosts of one shape.
xv6H drops that assumption: the host may change any of 24 facts under a
running guest, provided it tells the guest, lets it refuse what it cannot
give up, and keeps the old values working until the guest acknowledges the
new ones. Section 3 describes this in detail.

The build measured in the paper is commit `013e86c`, tagged `paper-build`.
The later commits add the scripts of the follow-up runs, this README, the
licences and tidied comments; they build the same hypervisor and guest
kernels byte for byte.

## RISC-V virtualisation with the H extension

This section covers the parts of the H extension that xv6H relies on. The
RISC-V Privileged Specification is the reference.

**Modes.** The H extension adds a virtualisation bit V to the privilege
levels. With V=0 the machine has M, HS (supervisor mode extended for a
hypervisor) and U. With V=1 it has VS, where a guest kernel runs, and VU,
where the guest's programs run. Traps taken while V=1 go to HS unless the
hypervisor delegates them to VS through `hedeleg` and `hideleg`. The guest
sees its own copies of the supervisor CSRs (`vsstatus`, `vsatp`, `vstvec`
and so on) under the usual names. xv6H's host kernel runs in HS and also
handles M mode itself: QEMU is started with `-bios none`, so no OpenSBI sits
underneath.

**Two-stage translation.** A guest's addresses are translated twice. The
VS stage, under the guest's control through `vsatp`, maps guest-virtual to
guest-physical addresses. The G stage, under the hypervisor's control
through `hgatp`, maps guest-physical to host-physical addresses. xv6H uses
Sv48x4 for the G stage when the host runs Sv48 and Sv39x4 otherwise. An
access to a guest-physical page the G stage does not map raises a
guest-page fault into HS, with the faulting guest-physical address in
`htval` and the trapping instruction in `htinst`. xv6H uses these faults for
three things: backing guest RAM on first touch (in 2 MB chunks), emulating
memory-mapped devices (their pages are never mapped, so each access traps
and is decoded), and tracking dirty pages during migration (RAM is
write-protected and each first write is recorded). The hypervisor reads and
writes guest memory through the guest's own translation with the `HLV` and
`HSV` instructions.

**Timers and interrupts.** The hypervisor injects virtual interrupts by
setting bits in `hvip`: VSSIP (supervisor software), VSTIP (timer) and VSEIP
(external). With the Sstc extension, a guest programs `vstimecmp` directly
and its timer fires without an exit; `htimedelta` offsets the guest's
`time`. Without Sstc, the guest sets its timer through the SBI TIME call.

**External interrupt controllers.** xv6H gives a guest one of three
controllers, all owned by the host:

- a **PLIC** or an **APLIC** in direct mode, emulated in software
  (`kernel/vplic.c`, `kernel/vaplic.c`): the guest's register accesses trap
  as guest-page faults, and pending interrupts are delivered through VSEIP;
- an **APLIC in MSI mode with an IMSIC** (`kernel/vimsic.c`). The AIA gives
  each physical hart a number of *guest interrupt files* (GEILEN of them).
  When the host has a free one, the vCPU's `hstatus.VGEIN` selects it and
  the guest's IMSIC page is mapped onto it through the G stage, so the guest
  claims and completes interrupts without exits. When no file is free, or
  the host has no AIA, the IMSIC is a software model: the guest's accesses
  to `vsiselect`, `vsireg` and `vstopei` trap and are emulated.

Every device model is level-triggered on a status the guest acknowledges, so
the host's share of the controller state is the set of asserted lines. That
property is what makes it safe to replace one controller with another under
a running guest. The CLINT is also modelled, for guests that send IPIs
through it.

**SBI.** A guest kernel calls its firmware with `ecall`, which traps from
VS to HS. xv6H answers as the firmware would (`kernel/hypsbi.c`): Base,
TIME, IPI, RFENCE, HSM, SRST, DBCN and the legacy console calls, in the SBI
v2.0 calling convention. It also implements one vendor extension, `MACH`
(`0x4D414348`), which carries the revision protocol described next.

## Revisable machine descriptions

### The facts and their classes

A RISC-V `virt`-class machine tells its guest a fixed set of things, mostly
through the device tree passed in `a1` at boot and the SBI extensions it
answers. xv6H makes each of them revisable; every node and property that the
host's device-tree builder (`kernel/fdtgen.c`) emits belongs to one of the
facts below.

Each fact has a class, which depends on the pair (fact, guest):

- **rebuilt**: the guest can rebuild everything that depends on the fact
  from the new description alone. The host only has to tell it.
- **negotiated**: some dependent state belongs to the guest (a task's vector
  registers, pages it is using). The host may only ask; the guest cedes or
  refuses.
- **carried**: a fact about the machine's history rather than its shape
  (the NIC's address, the disk's size), conveyed through the device itself
  to a driver that already re-reads it.
- **invisible**: what backs a fact the guest can see (hardware or software
  interrupt files, how guest RAM is backed). The host changes it and tells
  the guest nothing, because nothing observable changes.

The table lists the 24 facts. "Live" is the `vmm revise` word (or other
`vmm` command) that changes the fact under a running guest. "Host shape" is
the host boot word or `make` variable that makes a host present a different
value to its guests, which is how the tests create hosts of another shape
for migration. "Linux" says what part of a Linux 6.12 guest takes the
revision: a stock driver, the `machrev.ko` module alone, or one of the five
kernel patches (see [Linux guests](#linux-guests)).

| # | fact | class | live | host shape | Linux |
|---|---|---|---|---|---|
| F1 | interrupt controller kind | rebuilt | `plic`, `aplic`, `imsic` | `INTC=` | patch 5 |
| F2 | controller address | rebuilt | `intcmap alt\|default` | `guestintc=alt` | patch 5 |
| F3 | interrupt numbering | rebuilt | `irqmap alt\|default` | `guestirq=alt` | patch 2, module |
| F4 | IPI mechanism (CLINT or SBI) | rebuilt | `ipi sbi\|clint` | `guestipi=sbi` | stock |
| F5 | timer source (Sstc or SBI) | rebuilt | `timer sbi\|sstc` | `guesttimer=sbi` | patch 1 |
| F6 | timebase | rebuilt | `timebase HZ` | `guesttimebase=HZ` | patch 1 |
| F7 | hart count up | rebuilt | `cpus N` | `guestcpus=N` | module |
| F8 | hart count down | rebuilt | `cpus N` | `guestcpus=N` | module |
| F9 | hart numbering | rebuilt | `hartid SHIFT` | `guesthart=N` | patch 3 |
| F10 | vector unit present | negotiated | `vector`, `novector` | `VEC=0` | patch 4 |
| F11 | vector register width | negotiated | (crossing only) | `VLEN=` | patch 4 |
| F12 | vector unit revealed on first use | negotiated | `lazyvector` | `lazyv=1` | patch 4 |
| F13 | RAM up | rebuilt | `vmm resize VM MB` | `guestgrow=MB` | module |
| F14 | RAM down | negotiated | `vmm balloon VM MB` | `guestmem=MB` | stock (balloon) |
| F15 | device map | rebuilt | `map alt\|default` | `guestmap=alt` | patch 2 |
| F16 | RTC removed | rebuilt | `rtc off` | `guestrtc=off` | module |
| F17 | RTC restored | rebuilt | `rtc on` | | module |
| F18 | console kind | rebuilt | `console uart\|virtio\|sbi` | `guestcon=virtio\|sbi` | module |
| F19 | NIC identity | carried | `mac alt\|default` | `guestmac=alt` | patch 2 |
| F20 | disk capacity | carried | (crossing only) | a larger image | stock |
| F21 | cache-block size (`cbo.zero`) | rebuilt | `cboz BYTES` | `CBOZ=` | module |
| F22 | SBI extension set | rebuilt | `sbi LIST\|none` | `nosbi=EXT` | module |
| F23 | IMSIC in hardware or software | invisible | `vmm imsichw VM on\|off` | `AIA_GUESTS=` | none needed |
| F24 | memory backing | invisible | `vmm split VM`, `vmm ksm on\|off` | | none needed |

For a stock Linux, F1 and F2 are not rebuilt: its controller driver stays
bound to what it probed, so the host serves the old controller as a
software model instead.

### The protocol

A revision is a short exchange between the host and the guest over the
`MACH` SBI extension. It has eight calls:

| fid | call | meaning |
|---|---|---|
| 0 | `version()` | the protocol version; a guest that calls it is taken to speak the protocol |
| 1 | `pending()` | the current revision generation, for the guest to compare with its own |
| 2 | `describe()` | the guest-physical address of the new device tree |
| 3 | `ack(mask, gen)` | the generation the guest read and a mask of what it adopted (`DEVICES`, `INTC`, `VLEN`, `SBI`, `TIMER`, `MEM`, `MEMORY`); an acknowledgement of an older generation is refused |
| 4 | `dtb_max()` | the space a description may take |
| 5 | `request(mask)` | the guest asks for a fact it was not given, such as a vector unit on first use |
| 6 | `what()` | what the pending revision changed, so the guest can skip work |
| 7 | `rate_now()` | serve the new timebase from now on, called as the guest re-bases its clock |

A revision runs in three steps:

1. **Announce.** The host writes the new device tree into one of two
   description slots, which are declared as `reserved-memory` in the
   guest's tree. The tree the guest booted from is never rewritten (Linux
   keeps pointers into it), and a slot is reused only after the revision
   that used it is acknowledged. The host increments the generation and
   notifies every vCPU. For the xv6 guest the notification is a supervisor
   software interrupt with no IPI reason behind it, a channel that no
   revision can change. Linux uses its software interrupt for IPIs, so a
   Linux guest is notified through the *anchor*, a small device at
   `0x10005000` (compatible `xv6h,machrev`, line 5) that no revision ever
   moves or renumbers. The announcement is repeated until the guest reads
   the description. After a migration, a watchdog reports any arrival whose
   description is still unread after 20 seconds.
2. **Describe.** The first hart to notice calls `pending()` and
   `describe()`, parses the new tree into an empty description, compares it
   with the live one and applies what changed. The other harts bring up
   their own per-hart state (their interrupt file, for example). Each hart
   also compares its generation with the global one on every tick, so a
   lost notification is caught.
3. **Acknowledge.** The guest calls `ack(mask, gen)`, naming the generation
   it read and the classes it adopted. A guest can take the device map and
   decline the vector unit in the same answer.

The generation belongs to the machine, not the host: it travels in the
saved state, so a guest that was revised on one host and then moved is not
confused by the second host's counter.

### The alias invariant

Until the guest acknowledges a revision, it still runs against the old
description: its drivers touch the old device windows, its interrupt masks
name the old harts, its zeroing loop strides the old cache block. The host
therefore keeps every old value working until the acknowledgement:

> For every access the guest can make that is keyed by a revised fact, the
> host serves it correctly whether the guest uses the old value or the new
> one, until the guest acknowledges the new generation.

In xv6H this means that old device windows are still translated, the
outgoing controller's window stays as a "ghost" that reads zero and ignores
writes, old hart numbers still resolve, the software `cbo.zero` clears the
larger of the two block sizes, and the boot device tree is never written
over. One case cannot be aliased: when a device's old interrupt line is
another device's new one. For that case the host holds the device
interrupts whose numbering is changing from the moment the guest reads the
new description until it acknowledges, and raises them on the new numbers
afterwards. The timebase cannot be aliased either, since the host can serve
only one rate at a time; the guest calls `rate_now()` at the moment it
re-bases its clock.

`guest/aliasguest.c` and `tools/aliasstress.py` stress this window
deliberately.

### Tiers of guest, and negotiation

A guest that calls `version()` at boot is *participating*: on arrival at a
host of another shape it is built to the destination's shape and told
everything that differs before its first instruction there. A guest that
never calls `version()` is *legacy*: it is given the same description it
left with, and the destination provides whatever its own hardware lacks
through software models (the controller kind and device map it booted with,
a scaled timebase by trapping `time` reads and timer writes, SBI timers
instead of Sstc, a software `cbo.zero` that clears the described block
size). Legacy guests in this repository are a stock Linux, the module's
Linux booted with `machrev=off`, and the xv6 guest booted with `norevise` on
its command line.

A participating guest is *negotiating* for a fact it may have to give up.
The host cannot tell from outside whether a guest's tasks hold state in the
vector unit: the dirty bit it could sample is reset by the guest's own
kernel at every trap. So before moving a guest to a host with no vector
unit, or a narrower one, the source asks the guest to cede the unit. The
guest cedes it if no task holds vector state, or refuses and reports how
many tasks rely on it; the destination refuses a machine that arrives still
holding a unit it cannot honour, and the guest keeps running on the source.
Memory works the same way: a host with less room than the guest occupies
asks it to balloon down first.

On arrival, a participating guest is revised to the cheapest controller the
destination can offer: its own controller when it has hardware guest
interrupt files, and otherwise the kind the guest already had. The host
boot word `guestpolicy=native` makes the destination always revise to its
own controller.

A migration commits in two messages. The destination restores the machine,
keeps it stopped and answers *ready*; the source then sends *commit* and
destroys its copy, and only then does the destination run the guest. A
destination that loses the connection or waits five minutes without a
commit discards its copy, so one guest never runs on two hosts.

## Repository layout

| path | contents |
|---|---|
| `kernel/` | the kernel, host and guest in one binary. Hypervisor: `hyp.c` (VMs, vCPUs, G stage), `hypcsr.c`, `hypmmio.c`, `hypsbi.c`, `hypvec.S`; device models `vplic.c`, `vaplic.c`, `vimsic.c`, `vic.c` (the controller front end), `vclint.c`, `vuart.c`, `vrtc.c`, `vvirtio.c`, `vvnet.c`, `vvcon.c`, `vballoon.c`; the anchor `vmachrev.c`; the guest's device tree `fdtgen.c`; save, restore and migration `vmstate.c`. The guest side of the protocol is `machine.c`. M-mode trap handling for hardware that needs it (misaligned accesses on Rocket) is `mtrap.c`. The rest is the operating system: ext4 (`ext4_*.c`) and its journal (`jbd2.c`), the network stack (`net.c`, `tcp.c`, `sock*.c`, `bridge.c`, `conntrack.c`), the scheduler (`sched.c`, `rbtree.c`), namespaces, control groups and the system-call filter. |
| `user/` | the userland: about two hundred programs, the C library (`libc/`), the shell (`sh/`), the dynamic linker (`ldso/`), and `vmm.c`, the command that drives the hypervisor. |
| `mkfs/` | `mkext4.c`, which builds the ext4 host image and guest disks on the build machine, and `mkpasswd.c`. |
| `guest/` | bare guest payloads: `hello` (a non-xv6 guest linked at `0x80400000`, also built as a raw binary), `revguest` (an independent bare-metal guest of about three hundred lines that speaks the protocol), `aliasguest` (the alias-window stress), `fuzzguest` (random device, CSR and SBI abuse), `badguest` (a hostile guest), `fdguest` and `vguest` (floating-point and vector state across exits), `vmbench` (exit costs measured from inside a guest). |
| `linux/` | the Linux guest: `build.sh`, `build-stock.sh`, `build-https.sh`, `remote-build.sh`, `fetch.sh`, the module `machrev/`, the five patches `patches/` (with renderings for 6.18.54 in `patches/6.18.54/`), the controller driver of patch 5 in `vic/`, `patchcheck.py`, and small guest programs (`zerocheck.c`, `vecuser.c`, `vecprobe.c`, `crossck.c`, `maskbench.c`). |
| `kvm/` | the KVM implementation: `apply-kvm-patches.py` (KVM patches 1 to 4 against Linux 6.18.54), `apply-kvmtool.py` and `kvmtool/` (kvmtool's new files), `build-final.sh`, `build-d5.sh`, `mkhost.sh`, the harness `kvmh.py`, the tests `t_*.py`, and `dtsurvey.py`. |
| `fpga/` | the Rocket configuration (`rocket/XV6HConfigs.scala`, `rocket/bootrom.S`), RTL generation (`gen.sh`, `memgen.py`), synthesis (`synth.sh`, `synth_ooc.tcl`) and the ZedBoard shell and loader (`zedboard/`). |
| `model/` | the Promela models `revision.pml` and `revision_n.pml`, and the run scripts `run_r2.sh` and `run_n4.sh`. |
| `tools/` | the queue runner `qrun.py`, the build-box runner `remote.sh`, the measurement passes `night24.sh` and `post24.sh`, every measurement and endurance script of the paper, the board driver `zedboard.py`, and build helpers (`mkman.py`, `mkarchref.py`). |
| `bench/` | `hyp.py` (exit, scheduler and migration costs), `run.py` (file-system benchmark against Linux), `build-linux.sh`. |
| `test-xv6.py`, `test-hyp.py`, `test_hyp_*.py` | the test harness (see [Tests](#tests)). |
| `man/` | the manual pages, one per program, built into `/usr/share/man/man.db`. |
| `etc/`, `lib/`, `srv/` | files installed into the host image: `/etc` configuration and init scripts, shell functions for init, and the web server's pages. |
| `tls/` | BearSSL (`tls/bearssl`, under its own MIT licence), the root certificates and the generated trust anchors. |

## Building

### Toolchain

You need:

- A RISC-V 64-bit GCC and binutils. The Makefile looks for the prefixes
  `riscv64-unknown-elf-`, `riscv64-elf-`, `riscv64-none-elf-`,
  `riscv64-linux-gnu-` and `riscv64-unknown-linux-gnu-`, in that order;
  set `TOOLPREFIX=` to override. Ubuntu's `gcc-riscv64-linux-gnu` and
  Homebrew's `riscv64-elf-gcc` both work.
- QEMU built for `riscv64-softmmu`. The Makefile refuses versions older than
  7.2. Every result in the paper was produced with **QEMU 10.2.1**, which is
  the version to use. The two-host migration link uses
  `-netdev stream` with `reconnect-ms`, which older releases lack.
- A native `gcc` (for `mkfs/mkext4` and `mkfs/mkpasswd`), GNU `make`,
  `python3` (the image build runs `tools/mkman.py` and `tools/mkarchref.py`)
  and `bc` (the QEMU version check).

Known good: Ubuntu 26.04 on x86-64, where every measurement of the paper was
taken and where the CI job runs, and macOS with Homebrew's
`riscv64-elf-gcc`, which the CI also builds on.

### Build

```
make                  # kernel/kernel
make fs.img           # the host's ext4 image: user programs, manual, guest kernel and disks
make qemu             # both, then boot the host
make qemu-gdb         # boot stopped, waiting for gdb (see .gdbinit.tmpl-riscv)
make clean
```

The host image `fs.img` is ext4, built by `mkfs/mkext4`. It contains the
userland and, for guests, a stripped copy of the kernel (`/kernel`), guest
disks `/disk0.img` to `/disk2.img` (each its own 16 MB ext4 file system with
a 4 MB journal), and the bare payloads (`/hello`, `/hello.bin`, `/revguest`,
`/aliasguest`, `/fuzzguest`, `/badguest`, `/fdguest`, `/vguest`,
`/vmbench`). Image variables:

| variable | default | meaning |
|---|---|---|
| `HOSTFS_MB` | 96 | size of the host image in MB |
| `GUESTDISKS` | 3 | number of guest disks to put in it |
| `HOSTFILES` | (empty) | extra files to copy in, under their basename (Linux kernel and disk, for example) |

`make` does not notice a change of `HOSTFILES`; remove `fs.img` first.

## Running

### Booting the host

```
make qemu                       # PLIC, 3 harts, 1 GB   (-machine virt,aia=none)
make qemu INTC=aplic            # APLIC, direct mode    (aia=aplic)
make qemu INTC=imsic            # APLIC-MSI and IMSIC   (aia=aplic-imsic,aia-guests=2)
make qemu HYP=0                 # no H extension: plain xv6, no guests
```

Leave QEMU with `Ctrl-a x`. The launch variables combine freely:

| variable | default | meaning |
|---|---|---|
| `CPUS` | 3 | harts |
| `MEM` | 1G | RAM, up to 512G; the host keeps 128 MB for itself |
| `SV` | 48 | paging mode of the host and the G stage (48 or 39) |
| `INTC` | plic | `plic`, `aplic` or `imsic` |
| `AIA_GUESTS` | 2 | with `INTC=imsic`, hardware guest interrupt files per hart (0: software IMSIC only) |
| `HYP` | 1 | 0 hides the H extension |
| `VEC`, `VLEN` | 1, 128 | vector unit present, and its width in bits |
| `RVA23` | 1 | 0 drops the extra RVA23 user-profile extensions |
| `CBOZ` | 64 | the cache-block size `cbo.zero` clears |
| `NET` | 1 | 0 removes the network card |
| `FSIMG` | fs.img | the host image to boot from |
| `APPEND` | (empty) | words for the host's kernel command line |
| `LINK`, `LINKSRV` | | join two hosts by a unix socket (see below) |
| `EXTRADISK` | | a second disk image, seen as `/dev/vdb` |
| `MONITOR` | | QEMU's monitor on a unix socket at this path |

### Starting a guest

`vmm` is the front end to the hypervisor; `man vmm` on the running host
documents it. Some examples taken from the tests:

```
$ vmm start                                  # xv6 guest: /kernel, /disk0.img, 1 vCPU, 128 MB; Ctrl-] detaches
$ vmm start -n m1 -m 16 -c 1 -d              # named, 16 MB, detached
$ vmm start -n db -f /disk1.img -c 2 -d
$ vmm start -k /hello -f none -c 3           # a guest that is not xv6
$ vmm start -k /revguest -f none             # the independent guest that speaks the protocol
$ vmm start -n old -m 32 -c 1 -a norevise -d # an xv6 guest that behaves as a legacy guest
$ vmm ls
$ vmm attach db
$ vmm info db                                # state, exits, controller, revision generation, observed facts
$ vmm kill --all
```

Useful `vmm start` options: `-n NAME`, `-k KERNEL`, `-f DISK|none`,
`-c NCPU`, `-m MB`, `-a WORDS...` (the guest's command line, up to the next
option), `-r ADDR` (load a raw image at that address), `-d` (detached),
`-H` (secondary vCPUs wait for SBI HSM, as Linux expects), `-s` (software
IMSIC even when a hardware file is free), `-P`, `-A`, `-I` (ask for a
software PLIC, APLIC or IMSIC whatever the host has), `-V` (describe the
controller as one `xv6h,vic` node, for a Linux with patch 5), `-L` (no
vector unit until the guest asks for one), `-Q` (virtio console),
`-i dhcp|IP/PREFIX[,GW[,DNS]]` (the guest's address).

### Revising a fact

```
$ vmm revise VM imsic                 # F1: another controller kind (plic, aplic, imsic)
$ vmm revise VM intcmap alt           # F2: the controller's registers move
$ vmm revise VM irqmap alt            # F3: the interrupt lines are renumbered
$ vmm revise VM ipi sbi               # F4
$ vmm revise VM timer sbi             # F5
$ vmm revise VM timebase 1000000      # F6
$ vmm revise VM cpus 1                # F7/F8: one hart running, the rest parked
$ vmm revise VM hartid 4              # F9: harts numbered from 4
$ vmm revise VM novector              # F10 (and `vector`, `lazyvector` for F12)
$ vmm resize VM 192                   # F13
$ vmm balloon VM 32                   # F14: ask for 32 MB back
$ vmm revise VM map alt               # F15: device windows a megabyte up
$ vmm revise VM rtc off               # F16/F17
$ vmm revise VM console sbi           # F18
$ vmm revise VM mac alt               # F19
$ vmm revise VM cboz 128              # F21
$ vmm revise VM sbi rfence,hsm        # F22: withhold these (`none` withholds nothing)
$ vmm imsichw VM off                  # F23 (an IMSIC host); `on` returns to the hardware file
$ vmm split VM                        # F24: break 2 MB chunks into pages
$ vmm revise VM                       # ask the guest to re-read its machine
```

Each `alt` has a `default` that undoes it. The host prints a line such as
`revision G adopted (0xM) after T us` when the guest acknowledges. A
controller exchange requested while the guest is inside an interrupt
handler is refused with "try again".

### Migrating between two hosts

Two hosts are joined by a unix socket on the machine that runs both
emulators, so nothing listens on a network port. Each needs its own copy of
the image:

```
cp fs.img a.img; cp fs.img b.img
make qemu FSIMG=a.img LINK=/tmp/xv6h.sock LINKSRV=on      # host A, first
make qemu FSIMG=b.img LINK=/tmp/xv6h.sock LINKSRV=off     # host B, in another terminal
```

There is no DHCP server on this wire, so give the hosts addresses by hand:

```
A$ ip addr add 10.7.0.1/24 dev eth0
B$ ip addr add 10.7.0.2/24 dev eth0
A$ vmm start -n m1 -m 16 -c 1 -d
B$ vmm accept 9000 -d &
A$ vmm migrate m1 10.7.0.2:9000
```

Memory is sent while the guest runs, round after round, and the guest is
stopped only for the last pages. The disk does not travel: both hosts must
hold the same image under the same name.

To make B a host of another shape, give it boot words, for example
`make qemu FSIMG=b.img LINK=/tmp/xv6h.sock LINKSRV=off APPEND="guestmap=alt guesttimebase=1000000"`,
or a different `INTC`, `VEC`, `VLEN` or `CBOZ`. A participating guest
arriving there is told what changed; a legacy guest is served by models.

### Host command-line words

Passed with `APPEND="..."` (or `--append` in the test harness):

| word | effect |
|---|---|
| `guestintc=alt`, `guestmap=alt`, `guestirq=alt`, `guestipi=sbi`, `guesttimer=sbi`, `guesttimebase=HZ`, `guestcpus=N`, `guesthart=N`, `guestrtc=off`, `guestcon=virtio\|sbi`, `guestmac=alt`, `guestgrow=MB`, `guestmem=MB`, `lazyv=1` | describe a value other than the host's own to every guest: a host of another shape (see the fact table) |
| `nosbi=time\|ipi\|rfence\|hsm\|srst\|dbcn` | withhold an SBI extension from guests (F22); may be repeated |
| `guestpolicy=native` | revise an arriving participating guest to this host's own controller, rather than the cheapest one |
| `migkey=KEY` | authenticate migration streams with HMAC-SHA256; set the same key on both hosts |
| `norevise=1` | take machines of another shape and tell them nothing (the control for the paper's claim) |
| `nocbozmodel`, `tracetimer` | further controls: no software `cbo.zero`; print every translated timer deadline |
| `hostmem=MB` | memory the host keeps for itself |
| `sv39` | run the host in Sv39 (as `SV=39`) |

On a guest's command line (`vmm start -a ...`), `norevise` makes the xv6
guest never call `version()`, which is how the legacy tier is tested
without a second operating system. For the Linux guest, `machrev=off` does
the same.

## Linux guests

### Building the 6.12.41 guest

`linux/build.sh` runs on a Linux machine with `gcc-riscv64-linux-gnu`,
`curl`, `patch`, `cpio` and `mke2fs` (tested on Ubuntu 26.04). It downloads
Linux 6.12.41, BusyBox 1.37.0 and a musl cross toolchain (BusyBox and the
guest programs are built with musl at plain `rv64gc`), applies the five
patches, builds the kernel with an embedded initramfs, builds `machrev.ko`
against it and puts the module in the initramfs, and makes a 16 MB ext2
disk. A patch that does not apply stops the build. The script reads its
inputs from `~/xv6h-linux/src` and writes to `~/xv6h-linux/out`.

From another machine, `linux/remote-build.sh` copies the module, the patches,
the script and the guest programs to the build machine, runs the build and
fetches the results (`HOST=user@host linux/remote-build.sh`; `linux/fetch.sh`
fetches alone). To build on the Linux machine itself:

```
mkdir -p ~/xv6h-linux/src
cp -r linux/machrev linux/patches ~/xv6h-linux/src/
cp linux/build.sh linux/*.c user/kvlat.c ~/xv6h-linux/src/
bash ~/xv6h-linux/src/build.sh
cp ~/xv6h-linux/out/Image-hf linux/Image
cp ~/xv6h-linux/out/linux.img linux/linux.img
```

`linux/build-https.sh` (run before `build.sh`) adds the TLS client
`user/https.c` to the initramfs. Then boot a host with the kernel in its
image and start the guest:

```
rm -f fs.img
make qemu HOSTFS_MB=192 HOSTFILES="linux/Image linux/linux.img"
$ vmm start -n linux -k /Image -r 0x80200000 -m 256 -f /linux.img -c 2 -H -a console=ttyS0 earlycon=sbi
$ vmm start -n linux -k /Image -r 0x80200000 -m 256 -f /linux.img -c 2 -H -V -a console=ttyS0 earlycon=sbi
```

The second form describes the controller through patch 5's `xv6h,vic`
node, so the guest can follow F1 and F2. Adding `machrev=off` to the
command line gives the same image as a legacy guest.

### The 6.18.54 variant and the stock control

`TAG=-618 KVER=6.18.54 bash build.sh` builds Linux 6.18.54 beside the first
kernel, into `~/xv6h-linux/out-618`; two patches have their own renderings
for that version in `linux/patches/6.18.54/`. `linux/patchcheck.py VERSION`
dry-runs every patch against the pristine tarball.

`linux/build-stock.sh` builds a stock 6.12.41 with neither module nor
patches, configured from the patched guest's own `.config` and its
initramfs, into `~/xv6h-linux/out-stock`. It fails if any of the patches'
symbols is in the image. This kernel is the legacy-tier control. The script
expects the patched build's objects in `~/xv6h-linux/obj-hf` and the frozen
guest build in `~/xv6h-linux/out-freeze21` (a `build.sh` run with
`TAG=-freeze21` puts it there).

### The module

`linux/machrev/machrev.c` (1,002 lines) is an out-of-tree module. It binds
to the anchor (`xv6h,machrev`), announces itself with `version()`, and on
the anchor's interrupt reads the new device tree, compares it with what it
knows, applies what the kernel can apply, prints a `machrev:` line and
acknowledges with the mask of what it adopted. Through existing kernel
interfaces alone it takes the hart count in both directions (`add_cpu`,
`remove_cpu`; parked harts are described `xv6h,parked`), more RAM
(`add_memory`), the RTC leaving and returning, the console, the cache-block
size (the exported variable `clear_page` reads) and the SBI extension set
(a re-probe that flips the remote-fence static key). Three facts need
nothing at all, because stock drivers already re-read them: the SBI IPI
path, the balloon and the disk's capacity. The module links against the
patches' symbols, so it loads only on a patched kernel.

### The patches

Each patch covers facts that a stock kernel caches where a module cannot
reach. Together they add 994 non-blank lines.

| patch | facts | what it does |
|---|---|---|
| `0001-riscv-timer-revise` | F5, F6 | `riscv_timer_revise(sstc, hz)`: switches between Sstc and the SBI timer, moves the clocksource and every CPU's clock-event device to a new timebase, and re-arms every CPU |
| `0002-virtio-mmio-relocate` | F3, F15, F19 | `virtio_mmio_relocate()`: the driver's window and interrupt line follow a revised device map while the device and its queues stay put; `virtio_mmio_vdev()` lets the module read a device's configuration (its MAC address) |
| `0003-riscv-rebind-hart` | F9 | `riscv_rebind_hart()`: the CPU-to-hart map takes a new number; the PLIC context, the IMSIC interrupt file and every MSI that names the hart follow it, and the APLIC's hart-index width grows if needed |
| `0004-riscv-vector-revise` | F10, F11, F12 | `riscv_v_revise()`: cedes the vector unit when no task holds vector state and refuses with the count otherwise; a withdrawn unit cannot be started on; the first task that tries asks the host for it back; on return the save areas follow the width the hart reports |
| `0005-xv6h-vic` | F1, F2 | `drivers/irqchip/irq-xv6h-vic.c` (also in `linux/vic/`): a root interrupt controller bound to the description rather than to a driver, with PLIC, APLIC and APLIC-MSI plus IMSIC backends, re-chosen by `xv6h_vic_rebind()` when the module reads a revised kind or address |

## KVM and kvmtool

The protocol was implemented a second time, in KVM with kvmtool as the VMM,
with the same Linux guest and module unchanged. The KVM host is Linux
6.18.54 running under QEMU with an RVA23 CPU, the APLIC and IMSIC, and seven
guest interrupt files per hart.

**KVM patches** (`kvm/apply-kvm-patches.py`, applied idempotently to a Linux
6.18.54 tree):

1. forward the `MACH` SBI extension to the VMM, disabled until the VMM
   enables it;
2. make the timer's frequency writable on a running vCPU (F6), with `time`
   reads trapped while the guest's rate differs from the host's;
3. allow the vector extension's ISA register to change after the vCPU has
   run (F10);
4. move every vCPU's IMSIC between its hardware guest file and the software
   file under a running guest (F23).

**kvmtool** (upstream commit `f67bc0b`; new files in `kvm/kvmtool/`, edits
to existing files in `kvm/apply-kvmtool.py`): the anchor device, the two
description slots, the eight `MACH` calls, the facts F1, F3, F6, F10, F11
(refused, since the hardware vector length is fixed) and F15, the alias
window over KVM's in-kernel IMSIC, `ioeventfd`s and kvmtool's PLIC, the
`lkvm revise [--all] [-n name] <fact> [value]` command, and a stop-and-copy
migration over TCP, `lkvm migrate -n name --to ADDR:PORT`, that carries RAM,
the disk, every register KVM lists, the in-kernel APLIC and IMSIC state, the
PLIC, the devices and the revision state.

**Building.** `kvm/build-final.sh` builds the whole KVM stack into
`~/xv6h-kvm-final` on the build machine, after `kvm/` has been copied to
`~/xv6h-kvm-final/src`. It downloads nothing; it expects these in place:

- `~/xv6h-kvm/linux-6.18.54.orig`: a pristine Linux 6.18.54 tree, and
  `~/xv6h-kvm/build-host/.config`: a host kernel configuration with KVM and
  the AIA drivers;
- `~/xv6h-kvm/kvmtool`: a git clone of kvmtool at `f67bc0b` (the script
  checks the commit);
- `~/xv6h-kvm/sysroot`: `libfdt` built for riscv64 (`lib/libfdt.a` and its
  headers);
- `~/xv6h-kvm/hostroot/init`: the KVM host's init script, which
  `kvm/mkhost.sh` writes (that script refuses to run outside the author's
  build-box home, so on another machine copy its `init` by hand);
- `~/xv6h-linux/bb-hf` and `~/xv6h-linux/out-freeze21/Image-hf` with
  `~/xv6h-linux/out/linux.img`: BusyBox and the Linux guest from
  `linux/build.sh`.

It then patches and builds the host kernel with `riscv64-linux-gnu-`,
applies the kvmtool changes and builds `lkvm-static`, packs a host
initramfs with `lkvm` and the guest, and writes checksums to `BUILDSUMS`.
`kvm/build-d5.sh` is an earlier, separate build of a host with patch 4.

**Tests.** `kvm/kvmh.py` boots a KVM host under QEMU and drives it over its
console; each `t_*.py` is one experiment. Run them from
`~/xv6h-kvm-final/src` with the final stack selected:

```
export HOSTKERNEL=~/xv6h-kvm-final/build-host/arch/riscv/boot/Image
export HOSTINITRD=~/xv6h-kvm-final/host-initramfs.cpio.gz
python3 -u t_devmap.py 20 devmap,irqmap,intc
```

| script | what it checks |
|---|---|
| `t_proto.py` | the protocol calls end to end |
| `t_devmap.py N FACTS` | F15, F3 and F1 revised there and back N times, with the guest's disk checked after each; `IDLE_BEFORE=S` idles the guest first |
| `t_inflight.py N` | F1 under disk load |
| `t_timebase.py N` | F6 |
| `t_vector.py N` | F10 off and on |
| `t_imsicfile.py N` | F23: the IMSIC moved between hardware and software files |
| `t_migrate.py` | one migration; `INTC=plic` boots both sides on kvmtool's PLIC, `ARRIVE='intc plic;timebase 1000000'` revises on arrival |
| `t_migrate_bad.py` | damaged migration records are refused and the guest runs on |
| `t_cross.py MODE N` | N crossings between two KVM hosts, revised on each arrival (`intc`, `timebase`, `both`, `alt`) |
| `t_idle.py N` | the protocol's cost to an idle host |
| `t_mask.py N` | the masking baseline under KVM |

Logs go to `~/xv6h-kvm`.

## FPGA: Rocket with the H extension on a ZedBoard

`fpga/` holds a port of xv6H to real hardware: a single Rocket core
(rocket-chip `ece7b9a`) with the H extension, an FPU, 16 KB caches, a PLIC
and a CLINT, a 16550 UART and a RAM disk, on the Xilinx Zynq-7020 of a
ZedBoard. It uses about 35,300 LUTs and runs at 40 MHz. This host differs
from the QEMU hosts in hardware: a PLIC, a 400 kHz timebase, no Sstc, no
`time` CSR, no Zicboz, no vector unit, Sv39 only, software A/D bits,
misaligned accesses that trap, and one hart. On it a Linux guest takes live
revisions of the four facts that host can present (the controller's
address, the device map, the timebase and the SBI set), and guests move to
and from QEMU hosts by checkpoint over JTAG.

The flow uses `~/xv6h-fpga` on the build machine, with a rocket-chip
checkout in `~/xv6h-fpga/rocket-chip` and the contents of `fpga/` copied
into `~/xv6h-fpga`:

1. **Configuration.** `fpga/rocket/XV6HConfigs.scala` defines
   `XV6HZedConfig` (the core alone) and `XV6HZedSoCConfig` (the system the
   board shell wraps: RAM at `0x80000000`, devices at `0x10000000`, a boot
   ROM from `fpga/rocket/bootrom.S` that jumps to the kernel in DRAM with
   the device tree in `a1`). It goes into rocket-chip's
   `freechips.rocketchip.system` package.
2. **RTL.** `fpga/gen.sh [CONFIG]` elaborates the configuration with `mill`,
   lowers it with `firtool` and generates block-RAM implementations with
   `memgen.py` (Java 17 required).
3. **Synthesis and bitstream.** `fpga/synth.sh [CONFIG]` runs an
   out-of-context synthesis for a size and timing report;
   `fpga/zedboard/build.sh [CONFIG]` builds the board design and the
   bitstream. Both run **Vivado 2020.1** in the Docker image
   `kkrizka/vivado:2020.1`.
4. **The board.** `tools/zedboard.py [steps]` builds the images (the kernel
   as a raw binary with its `.bss`, the board's device tree from
   `fpga/zedboard/mkdts.py`, a 90 MB host image with an xv6 guest disk and
   the Linux guest), builds the Arm-side UART bridge
   (`fpga/zedboard/arm/`, with `arm-none-eabi-gcc`), opens the board's
   USB-UART (`/dev/ttyACM0`), and loads everything over JTAG with `xsdb`
   (`fpga/zedboard/load.tcl`). Steps: `host`, `guest`, `linux` (the default
   is all three), `revise` (with `rounds=N`), `save` and `restore`.
   `fpga/zedboard/lx` and `lxv` are the `vmm start` lines for the Linux
   guest, the second with the description-bound controller.

`tools/b4_depart.py` and `tools/b4_arrive.py` are the QEMU halves of the
moves between QEMU and the board. `fpga/zedboard/debug.sh`, `ila.tcl` and
`ilasum.py` are the integrated-logic-analyser tooling used while porting.

## Model checking

Two Promela models check the protocol with Spin (6.5.2 was used).

`model/revision.pml` models one machine of two harts, one revised fact, two
hosts between which the machine may cross, announcements that may be lost,
and the alias. It checks *safety* (every access keyed by the fact is served
correctly, and the generation is only ever the host's), *adopted* (every
revision is eventually acknowledged) and *caught up* (every hart eventually
brings its own state up to date). Compile-time variants reproduce earlier
versions of the host and guest that failed: `-DCROSS_MID`,
`-DCARRY_ALIAS`, `-DACK_NAMES_GEN`, `-DNOREPEAT` and `-DTICKCHECK`, each
described in the file's header. To run it:

```
spin -a revision.pml
gcc -O2 -DSAFETY -o pan pan.c && ./pan                 # safety
gcc -O2 -o pan pan.c && ./pan -a -f -N adopted         # liveness, weak fairness
./pan -a -f -N caught_up
```

`model/revision_n.pml` generalises to `NH` harts and `NF` facts, with owed
facts joining the next revision, and models the timebase, which cannot be
aliased, with `MACH` call 7. It adds the property *no owed* (every fact
asked for is eventually acknowledged). The variants `-DRATE_AT_ASK` and
`-DNO_CALL7` must fail safety.

```
spin -a -DNH=3 -DNF=2 revision_n.pml
gcc -O2 -DSAFETY -o pan pan.c && ./pan
gcc -O2 -DNFAIR=6 -o pan pan.c && ./pan -a -f -N adopted     # and no_owed, caught_up
```

`model/run_r2.sh` runs the set reported in the paper: two harts and two
facts, all four properties exhaustively; the two variants that must fail;
and three harts with two facts, safety exhaustively under `COLLAPSE` with a
60 GB memory limit and liveness by bitstate. It writes into the current
directory and can be restarted; finished properties are not rerun.
`model/run_n4.sh` extends this to three harts with three facts and four
harts with two. Both put `~/spin/Spin-master/Src` at the front of `PATH`,
so a `spin` already on `PATH` also works.

## Tests

### xv6's own tests

`./test-xv6.py usertests` (add `-q` for the quick set) and
`./test-xv6.py crash` run xv6's original tests without a person at the
console. This is what the CI runs.

### The hypervisor harness

`test-hyp.py` boots one or two emulated hosts per test, drives them through
their consoles, and waits for markers in the output. The tests live in
`test-hyp.py` and `test_hyp_*.py` and are grouped in ten suites:

| suite | what it covers |
|---|---|
| `native` | the kernel and userland on each controller kind, without the H extension, without a vector unit, with wide vectors, plus file systems, users, init, the manual |
| `ns` | namespaces |
| `ct` | control groups, containers, the system-call filter, container networking |
| `guest` | xv6 guests: boot, `usertests` inside guests, isolation, cloning, ballooning, same-page merging, hostile and fuzzing guests, and every live revision (`guest-revise-*`, `guest-invisible-*`, `guest-revguest`) |
| `linux` | Linux guests: boot, SMP disk work, participation and every live revision (`lx-revise-*`, `lx-invisible-*`) |
| `net` | the network stack, DHCP and static addresses, gateways and published ports |
| `mm` | 1 GB, Sv39, 16 GB and 512 GB machines |
| `proc` | thousands of processes, pid wraparound |
| `sched` | fairness, latency, affinity, groups, VM weights |
| `snap` | save, restore, clone, and migration: every fact across a crossing (`snap-migrate-*` for the xv6 guest, `snap-lx-migrate-*` for Linux), with `-legacy` variants, refused and negotiated moves, and several facts at once (`-multi4`, `-multi8`) |

```
./test-hyp.py                          # every suite, on every controller kind
./test-hyp.py snap                     # one suite
./test-hyp.py -t 'guest-revise'        # tests whose name matches a pattern
./test-hyp.py -i imsic -c 4            # one controller kind, four harts
./test-hyp.py --tier quick             # thirty-six tests, one of each kind
./test-hyp.py --axis mem=16G,sv=39     # the same tests on another machine (intc, cpus, mem, hyp, vec, vlen, sv)
./test-hyp.py --append "guestpolicy=native"   # words for every host's command line
./test-hyp.py --list                   # names only
./test-hyp.py --json out.json          # results with durations
```

`-v` copies QEMU's output to the terminal; otherwise it goes to
`test-hyp.log`. `--no-build` skips the build. The Linux tests are skipped
unless `linux/Image` and `linux/linux.img` exist.

### Queues

`tools/qrun.py` runs each test as its own job, several at a time, with its
own disk image and console log, so a matrix takes as long as its slowest
test. Jobs are classed as *exclusive* (timing tests, run last and alone),
*heavy* (long, CPU-bound), *huge* (the 16 GB and 512 GB machines, the large
process tests) and *light*.

```
tools/qrun.py                               # the whole matrix
tools/qrun.py -s guest                      # one suite (repeatable)
tools/qrun.py -t guest-revise -r 20         # tests whose name contains this, twenty times each
tools/qrun.py --slots 6                     # at most six jobs at once
tools/qrun.py --capacity 24 --timeout 900   # harts to keep busy; seconds per light job
tools/qrun.py --resume                      # skip what qresults.txt already records
```

`--axis`, `--tier` and `--json` are passed on as for `test-hyp.py`, and
`--progress FILE` names the results file. Each job's output is
`qlogs/TEST.RUN.out`, and the running results are in `qresults.txt`.
`tools/campaign.py` runs the quick tier on ten machine shapes and then the
whole matrix with the benchmarks.

### Runs on a build machine

`tools/remote.sh` copies the tree into its own directory
`~/xv6h-runs/NAME` on a build machine, builds it there and starts a run as a
user systemd unit, so that several versions can be tested at once and runs
survive the session that started them.

```
tools/remote.sh run NAME SUITE [test-hyp.py args]     # a suite
tools/remote.sh queue NAME [qrun.py args]             # a queue
tools/remote.sh script NAME FILE.py [args]            # a measurement script
tools/remote.sh build NAME                            # copy and build only
tools/remote.sh status [NAME] | wait NAME | log NAME | kill NAME | rm NAME
tools/remote.sh resume NAME [qrun.py args]            # continue a queue
tools/remote.sh clean                                 # strip finished runs to their results
```

`HOST=user@host` selects the machine (over ssh with keys, `rsync` and
`systemd-run --user`; enable lingering for the user). `HOST=local` runs on
the machine you are on. `SEED=NAME` copies an earlier run's tree instead of
the working tree, which freezes a build for a sequence of runs.
`LXTAG=TAG` picks the Linux image from `~/xv6h-linux/outTAG/Image-hf`
(`-618` for Linux 6.18.54, `-stock` for the stock control, `-freeze21` for
the frozen guest); the disk always comes from `~/xv6h-linux/out/linux.img`.
A `script` run also builds `fs.img` with the Linux kernel and disk in it.

A run directory holds the tree and these results:

| file | contents |
|---|---|
| `BUILDSTAMP` | the commit, the number of uncommitted files, and the date |
| `build.out` | the build's output |
| `results.out` | the run's output |
| `done` | the exit status, written when the run ends |
| `load.log` | the load average, sampled every 30 seconds through the run |
| `qresults.txt`, `qlogs/` | for queues: one line per job, and each job's output |
| `run_script.py` | for scripts: the script that ran |

## Reproducing the paper's results

All results in the paper come from one build, commit `013e86c` (tag
`paper-build`), run on one
32-core Linux machine with QEMU 10.2.1. Every QEMU-based number is an
emulated time; absolute timings depend on the host and on its load, which is
why every run records `load.log`. Compare ratios and shapes rather than
absolute values. The raw logs of the paper's runs are too large for this
repository and are available from the author on request.

### Preparation

On the build machine, with this repository checked out:

1. Build the Linux guest (see [Linux guests](#linux-guests)) into
   `~/xv6h-linux/out`, and the frozen copy with `TAG=-freeze21`. For the
   Linux 6.18.54 rows also `TAG=-618 KVER=6.18.54`, and for the stock
   control `linux/build-stock.sh`.
2. For the KVM rows, build the KVM stack with `kvm/build-final.sh`.
3. For the FPGA rows, build the bitstream as described under
   [FPGA](#fpga-rocket-with-the-h-extension-on-a-zedboard) and connect the
   ZedBoard by JTAG and USB-UART.
4. `export HOST=local`, so that `tools/remote.sh` runs on this machine.

In the commands below, `NAME` is any run name you choose. A queue's per-test
results are in the run's `qresults.txt` and `qlogs/`; a script prints its
table to `results.out`.

### The whole sequence

`tools/night24.sh` runs the first pass: the timing runs one at a time on a
quiet machine (it waits up to 30 minutes for the one-minute load average to
fall below 5), then two chains side by side (the test queues, and the
endurance loops two at a time), then the KVM runs one at a time.
`tools/post24.sh` runs the follow-up measurements. Both are resumable: they
record finished stages in `~/xv6h-runs/night24.state` (or `post24.state`),
resume a queue that died and restart a script that died. `night24.sh` uses
the frozen tree `~/xv6h-runs/freeze24`, which `HOST=local tools/remote.sh
build freeze24` makes; `post24.sh` uses `freeze24p` the same way, and
`freeze21`, an earlier build that is not part of this repository, for one
masking comparison. Both use the Linux image `LXTAG=-freeze21`.

### Table by table

Below, `remote.sh` stands for `tools/remote.sh`. The KVM commands run in
`~/xv6h-kvm-final/src` with `HOSTKERNEL` and `HOSTINITRD` set as shown under
[KVM](#kvm-and-kvmtool), each as `python3 -u SCRIPT ARGS`; an environment
setting shown before a KVM script is exported for that run.

**Correctness: every fact revised**

- Per-fact results, xv6 guest across crossings, and the crossing's timeline:
  `remote.sh queue NAME --repeat 20 -t migrate --slots 6`, then
  `tools/timeline_table.py qlogs` in the run directory.
- Per-fact results, xv6 guest, live:
  `remote.sh queue NAME -t guest-revise -r 20 --slots 4`.
- Per-fact results, Linux guest, live:
  `remote.sh queue NAME -s linux -r 20 --slots 8`.
- Per-fact results, Linux guest across crossings:
  `remote.sh queue NAME -t snap-lx-migrate -r 100 --slots 8`.
- Per-fact results, KVM column: `t_devmap.py 20 devmap,irqmap,intc`;
  `t_devmap.py 10 intc` and `IDLE_BEFORE=10 t_devmap.py 10 intc` (the
  guest busy and idle); `t_inflight.py 20`; `t_timebase.py 8`;
  `t_vector.py 20`; `t_imsicfile.py 20`.
- Per-fact results, FPGA column, and booting on the board:
  `remote.sh script NAME tools/zedboard.py revise rounds=10`;
  `remote.sh script NAME tools/zedboard.py host guest linux`.
- Moves between the board and QEMU by checkpoint: board to QEMU,
  `tools/zedboard.py host save` then `tools/b4_arrive.py`; QEMU to board,
  `tools/b4_depart.py 0 sv39` then `tools/zedboard.py host restore` (each
  run with `remote.sh script NAME ...`).
- Several facts at once: `remote.sh queue NAME --repeat 20 -t migrate-multi --slots 6`.
- Revision under KVM, migration rows: `t_proto.py`; `t_migrate.py`, with
  `INTC=plic` and with `ARRIVE='intc plic;timebase 1000000'`;
  `t_migrate_bad.py`; `t_cross.py intc 20`, `t_cross.py timebase 20`,
  `t_cross.py alt 300`.
- The independent protocol guest:
  `remote.sh queue NAME -t guest-revguest -r 20 --slots 2`.
- The full suite: `remote.sh queue NAME --slots 6`.
- The campaign over ten machine shapes: `remote.sh script NAME tools/campaign.py`.
- The big-memory machines:
  `remote.sh queue NAME -t mm-16g -r 3 --slots 1` and
  `remote.sh queue NAME -t mm-512g -r 3 --slots 1`.
- The TLB-flush storm during revision:
  `remote.sh run NAME guest -t guest-revise-flushstorm`.
- Linux 6.18.54: `LXTAG=-618 remote.sh run NAME linux` and
  `LXTAG=-618 remote.sh run NAME snap -t snap-lx-migrate-`.
- Two disk loops on two vCPUs:
  `remote.sh queue NAME -t linux-disk-smp -r 20 --slots 1`, and the same
  timed three ways with `remote.sh script NAME tools/smptime.py 10`.

**Latency and endurance**

- Per-fact adoption latency (table and left plot):
  `remote.sh script NAME tools/adoptprobe.py xv6 20`, and the same with
  `linux`, `linuxvic` and `xv6+map` in place of `xv6`.
- Endurance runs, the long-run plots and adoption latency on arrival:
  `remote.sh script NAME tools/hunt2.py 1000`,
  `remote.sh script NAME tools/hunt4.py 500`,
  `remote.sh script NAME tools/hunt3.py 1000`, and the ring of three
  shapes `remote.sh script NAME tools/grandtour.py 100`.
- Adoption latency under load: `remote.sh script NAME tools/revload.py 5`.
- A line held at the controller exchange is delivered:
  `remote.sh script NAME tools/heldline.py 16`.
- What a guest observes, and when:
  `remote.sh script NAME tools/obstime.py xv6` and the same with `linux`.

**Controls and baselines**

- The control, a crossing without revision (and the listing it prints):
  `remote.sh script NAME tools/control.py 3`.
- The first crossings of a stock Linux:
  `LXTAG=-stock remote.sh script NAME tools/stockcross.py 3 imsic2plic`
  (the default direction is `plic2imsic`), and the queues
  `LXTAG=-stock remote.sh queue NAME -t linux-boot -r 20 --slots 6` and
  `LXTAG=-stock remote.sh queue NAME -t snap-lx-migrate-intc-legacy -r 20 --slots 4`.
- The masking baseline on xv6H: `remote.sh script NAME tools/maskfloor.py 5`;
  under KVM: `t_mask.py 5`.
- A guest booted at the masking floor against one revised down to it:
  `remote.sh script NAME tools/maskboot.py 5`.
- QEMU's own live migration on the same moves:
  `remote.sh script NAME tools/qemubase.py 3`.

**Cost**

- Exits per workload by tier (table and left cost plot):
  `remote.sh script NAME tools/tiercost.py 5`.
- What moving to the cheaper controller gains:
  `remote.sh script NAME tools/revcost.py 12`.
- The two tiers on the same crossing:
  `remote.sh script NAME tools/tiercross.py 4`.
- What the protocol costs an idle host:
  `remote.sh script NAME tools/idlecost.py 10`; under KVM `t_idle.py 10`.
- Scaling of adoption latency, one axis at a time:
  `remote.sh script NAME tools/scale.py console xv6 20`,
  `tools/scale.py vcpus xv6 20`, `tools/scale.py vcpus linux 20`,
  `tools/scale.py ram xv6 10`, `tools/scale.py ram linux 10`,
  `tools/scale.py load xv6 20`, `tools/scale.py devices xv6 20` (each as a
  `remote.sh script` run).
- Migration downtime and total time against RAM and dirtying rate:
  `remote.sh script NAME tools/migscale.py 3`.
- The system's own costs (exits, scheduler, migration; middle cost plot):
  `remote.sh script NAME bench/hyp.py`, three runs.
- The file-system benchmark against Linux (right cost plot):
  `remote.sh script NAME bench/run.py`.

**Applications, fleets and threats**

- What a key-value service in a Linux guest sees during revisions:
  `remote.sh script NAME tools/taillat.py 5 500 all split`.
- The same service across crossings: `tools/taillat_mig.py same 25`,
  `tools/taillat_mig.py timebase 10` and `tools/taillat_mig.py intc 10`
  (each as a `remote.sh script` run).
- Two fleet decisions end to end:
  `remote.sh script NAME tools/scenario.py upgrade` and
  `remote.sh script NAME tools/scenario.py consolidate`.
- The alias-window stress: `remote.sh script NAME tools/aliasstress.py 200`.
- Adoption beside a fuzzed host: `remote.sh script NAME tools/fuzzco.py 20 300000`.
- The authenticated migration stream and a replayed stream:
  `remote.sh script NAME tools/migmac.py 64 20` and
  `remote.sh script NAME tools/migreplay.py keyed,plain 16`.

**Model, survey and sizes**

- The model-checking tables: `model/run_r2.sh`, and `model/revision.pml`
  as in its header (see [Model checking](#model-checking)).
- The survey of device trees: `python3 kvm/dtsurvey.py OUT.md DTB_DIR...`
  over the device trees (`*.dtb`) built from Linux 6.18.54's RISC-V
  sources.
- The kernel's size by subsystem: `tools/ksizes.py 013e86c`, which
  reproduces the paper's table exactly.

The Spin models and the device-tree survey do not depend on the build. The
defects discussed in the paper were fixed before `013e86c`; this
repository's history condenses that work into one or a few commits per
working day.

## Licence

The hypervisor, its kernel and userland, the guests, the tools and the
scripts are under the MIT licence. xv6's copyright notice (Frans Kaashoek,
Robert Morris, Russ Cox, Massachusetts Institute of Technology) is retained,
with Copyright (c) 2026 Jyotiprakash Mishra for the additions; see
[`LICENSE`](LICENSE).

The Linux module and patches in `linux/` and the KVM kernel patches in
`kvm/` are GPL-2.0-only, as is the kernel they modify. The kvmtool changes
in `kvm/kvmtool/` and `kvm/apply-kvmtool.py` are GPL-2.0, as kvmtool is.
BearSSL in `tls/bearssl` keeps its own MIT licence
(`tls/bearssl/LICENSE.txt`). The licence texts are in
[`LICENSES/`](LICENSES/).

## Citation

If you use xv6H, please cite:

J. Mishra, A. Pathak, S. K. Sahay, L. K. Vashishtha, S. Mishra, *Revisable
Machine Descriptions: Twenty-Four Facts a Running Guest Can Be Told Again*
(under review).

```bibtex
@unpublished{mishra-revisable,
  author = {Jyotiprakash Mishra and Aman Pathak and Sanjay K. Sahay
            and Lalit K. Vashishtha and Swati Mishra},
  title  = {Revisable Machine Descriptions: Twenty-Four Facts a Running
            Guest Can Be Told Again},
  note   = {Under review}
}
```

Author: Jyotiprakash Mishra, <mail@jyotiprakash.org>.
