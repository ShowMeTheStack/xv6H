K=kernel
U=user

OBJS = \
  $K/entry.o \
  $K/start.o \
  $K/console.o \
  $K/printk.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trampoline.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/fs.o \
  $K/log.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/kernelvec.o \
  $K/plic.o \
  $K/virtio_disk.o \
  $K/fdt.o \
  $K/platform.o \
  $K/mvec.o \
  $K/intc.o \
  $K/aplic.o \
  $K/imsic.o \
  $K/hyp.o \
  $K/hypvec.o \
  $K/hypcsr.o \
  $K/hypmmio.o \
  $K/vic.o \
  $K/vuart.o \
  $K/vvirtio.o \
  $K/vclint.o \
  $K/vplic.o \
  $K/vaplic.o \
  $K/vimsic.o \
  $K/fdtgen.o \
  $K/syshyp.o \
  $K/fpu.o \
  $K/hypsbi.o \
  $K/virtio_net.o \
  $K/net.o \
  $K/bridge.o \
  $K/vvnet.o \
  $K/rtc.o \
  $K/vrtc.o \
  $K/sock.o \
  $K/tcp.o

# riscv64-unknown-elf- or riscv64-linux-gnu-
# perhaps in /opt/riscv/bin
#TOOLPREFIX = 

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-none-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

QEMU = qemu-system-riscv64
MIN_QEMU_VERSION = 7.2

CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# Deterministic builds.
DETFLAGS = -ffile-prefix-map=$(CURDIR)=.

CFLAGS = -Wall -Werror -Wno-unknown-attributes -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += $(DETFLAGS)
CFLAGS += -march=rv64gc$(MARCH_EXTRA)
# the kernel uses hypervisor-extension instructions and CSR names.
KMARCH = -march=rv64gch_zicsr_zifencei
CFLAGS += -std=gnu99
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096

$K/%.o: $K/%.c
	$(CC) $(CFLAGS) $(KMARCH) -c -o $@ $<

$K/%.o: $K/%.S
	$(CC) $(CFLAGS) $(KMARCH) -c -o $@ $<

$K/kernel: $(OBJS) $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) 
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) -march=rv64gc -g $(DETFLAGS) -c -o $@ $<

tags: $(OBJS)
	etags kernel/*.S kernel/*.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o $U/net.o $U/time.o

# kernel objects see kernel-only definitions (memlayout.h).
$K/%.o: CFLAGS += -DKERNEL

# mandel's checksum assumes IEEE double results without fused multiply-add.
$U/mandel.o: CFLAGS += -ffp-contract=off

_%: %.o $(ULIB) $U/user.ld
	$(LD) $(LDFLAGS) -T $U/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

# ---- TLS: BearSSL (tls/bearssl) as an xv6 user library, and the https client ----
AR = $(TOOLPREFIX)ar
TLSSRC = $(wildcard tls/bearssl/src/*/*.c) tls/bearssl/src/settings.c
TLSOBJ = $(patsubst tls/bearssl/src/%.c,tls/obj/%.o,$(TLSSRC)) tls/obj/trust_anchors.o
TLSCFLAGS = $(filter-out -O,$(CFLAGS)) -O2 -Itls/bearssl/inc -Itls/bearssl/src -Itls/shim \
	-DBR_64=1 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0

tls/obj/%.o: tls/bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TLSCFLAGS) -c -o $@ $<

tls/obj/trust_anchors.o: tls/trust_anchors.c
	@mkdir -p $(dir $@)
	$(CC) $(TLSCFLAGS) -c -o $@ $<

tls/libbearssl.a: $(TLSOBJ)
	rm -f $@
	$(AR) rcs $@ $^

# xv6 has no clock: the https client checks certificate validity against
# the build date (rounded to the hour, so this rarely changes).
$U/tls_date.h: FORCE
	@t=$$(date -u +%s); t=$$(( t / 3600 * 3600 )); \
	printf '// generated by make: build time for certificate validity checks\n#define TLS_BUILD_SECS %sUL\n' $$t > $@.tmp; \
	cmp -s $@.tmp $@ 2>/dev/null && rm -f $@.tmp || mv $@.tmp $@
FORCE:

$U/https.o: CFLAGS += -Itls/bearssl/inc -Itls/shim
$U/https.o: $U/tls_date.h
$U/_https: $U/https.o $(ULIB) tls/libbearssl.a $U/user.ld
	$(LD) $(LDFLAGS) -T $U/user.ld -o $@ $U/https.o $(ULIB) tls/libbearssl.a
	$(OBJDUMP) -S $@ > $U/https.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $U/https.sym

$U/_forktest: $U/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -Wno-unknown-attributes -I. -o mkfs/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_logstress\
	$U/_forphan\
	$U/_dorphan\
	$U/_sync\
	$U/_vmm\
	$U/_cp\
	$U/_fpbench\
	$U/_vecbench\
	$U/_mandel\
	$U/_ifconfig\
	$U/_ping\
	$U/_nslookup\
	$U/_wget\
	$U/_https\
	$U/_date\

# ---- guest images ----
# everything a guest needs lives in the host file system (fs.img):
#   /kernel        stripped copy of this kernel (the default guest kernel)
#   /diskN.img     guest disk images: copies of guest/disk.img, an xv6 file
#                  system with the user programs (GUESTDISKS of them)
#   /badguest      hostile test guest, /hello and /hello.bin: a non-xv6 guest
#   $(HOSTFILES)   any extra files you name (basenames must be <= 14 chars),
#                  e.g. make fs.img HOSTFILES="mykernel.elf myfs.img"
# HOSTFS_MB sizes the host image (files up to 128 MB are supported).
HOSTFS_MB ?= 64
GUESTDISKS ?= 3
GUESTFS_BLOCKS ?= 6000   # blocks per guest disk: the programs (https is big) plus room for usertests
HOSTFILES ?=

guest/kernel: $K/kernel
	$(OBJCOPY) -S $K/kernel guest/kernel

guest/disk.img: mkfs/mkfs README $(UPROGS)
	mkfs/mkfs -s $(GUESTFS_BLOCKS) guest/disk.img README $(UPROGS)

GUESTDISKIDX = $(shell i=0; while [ $$i -lt $(GUESTDISKS) ]; do echo $$i; i=$$((i+1)); done)
GUESTDISKIMGS = $(foreach k,$(GUESTDISKIDX),guest/disk$(k).img)
guest/disk%.img: guest/disk.img
	cp guest/disk.img $@

# a guest that is not xv6: linked at 0x80400000, also as a raw binary.
guest/hello: guest/hello.c guest/hello.S guest/hello.ld
	$(CC) -Wall -Werror -O -march=rv64g -mcmodel=medany -ffreestanding -fno-common -nostdlib \
	  -fno-stack-protector -fno-pie -no-pie -Wl,-T,guest/hello.ld -o guest/hello \
	  guest/hello.S guest/hello.c
guest/hello.bin: guest/hello
	$(OBJCOPY) -O binary guest/hello guest/hello.bin

# bare-metal guests exercising floating point and vectors.
guest/fdguest: guest/fdguest.c guest/hello.S guest/hello.ld
	$(CC) -Wall -Werror -O -march=rv64g -mabi=lp64d -mcmodel=medany -ffreestanding -fno-common -nostdlib \
	  -fno-stack-protector -fno-pie -no-pie -Wl,-T,guest/hello.ld -o guest/fdguest \
	  guest/hello.S guest/fdguest.c
guest/vguest: guest/vguest.c guest/hello.S guest/hello.ld
	$(CC) -Wall -Werror -O -march=rv64g -mabi=lp64d -mcmodel=medany -ffreestanding -fno-common -nostdlib \
	  -fno-stack-protector -fno-pie -no-pie -Wl,-T,guest/hello.ld -o guest/vguest \
	  guest/hello.S guest/vguest.c

# a hostile test guest (see guest/badguest.c), used by test-hyp.py.
guest/badguest: guest/badguest.c guest/badentry.S guest/badguest.ld
	$(CC) -Wall -Werror -O -march=rv64g -mcmodel=medany -ffreestanding -fno-common -nostdlib \
	  -fno-stack-protector -fno-pie -no-pie -Wl,-T,guest/badguest.ld -o guest/badguest \
	  guest/badentry.S guest/badguest.c

GUESTBINS = guest/kernel guest/badguest guest/hello guest/hello.bin guest/fdguest guest/vguest
fs.img: mkfs/mkfs README $(UPROGS) $(GUESTBINS) $(GUESTDISKIMGS) $(HOSTFILES)
	mkfs/mkfs -s $$(( $(HOSTFS_MB) * 1024 )) fs.img README $(GUESTBINS) $(GUESTDISKIMGS) $(UPROGS) $(HOSTFILES)

-include kernel/*.d user/*.d tls/obj/*.d tls/obj/*/*.d

clean: 
	rm -rf tls/obj
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$K/kernel fs.img test-fs.img guest/kernel guest/badguest guest/hello guest/hello.bin guest/fdguest guest/vguest guest/disk*.img \
	mkfs/mkfs .gdbinit \
        $U/usys.S $U/tls_date.h tls/libbearssl.a \
	$(UPROGS)

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 3
endif
# RAM: the host keeps HOSTMEM_MB (default 128) for itself; the rest is
# handed out to guest VMs.
MEM ?= 1G
# interrupt controller: plic | aplic | imsic  (qemu -machine virt,aia=...)
INTC ?= plic
AIA_GUESTS ?= 2
ifeq ($(INTC),plic)
AIAOPT = aia=none
else ifeq ($(INTC),aplic)
AIAOPT = aia=aplic
else ifeq ($(INTC),imsic)
AIAOPT = aia=aplic-imsic,aia-guests=$(AIA_GUESTS)
else
$(error INTC must be plic, aplic, or imsic)
endif

# HYP=0 hides the hypervisor extension from the CPU: plain xv6, no guests.
# VEC=0 removes the vector extension; VLEN sets its register width in bits.
HYP ?= 1
VEC ?= 1
VLEN ?= 128
ifeq ($(HYP),0)
CPUOPT = -cpu rv64,h=false
else
CPUOPT = -cpu rv64
endif
ifeq ($(VEC),0)
CPUOPT := $(CPUOPT),v=false
else
CPUOPT := $(CPUOPT),v=true,vlen=$(VLEN)
endif
# RVA23=1 (default) adds the rest of the RVA23 user profile (bit
# manipulation, Zcb, Zicond, Zfa, Zfhmin, vector crypto/bf16, cache ops,
# pointer masking), so that distribution userlands built for RVA23 run
# in guests. none of these carry state the hypervisor must switch.
RVA23 ?= 1
ifeq ($(RVA23),1)
CPUOPT := $(CPUOPT),zba=true,zbb=true,zbs=true,zcb=true,zicond=true,zfa=true,zfhmin=true,zimop=true,zcmop=true,zawrs=true,zicbom=true,zicboz=true,zicbop=true,zihintntl=true,zihintpause=true,zicntr=true,zihpm=true,zkt=true,supm=true
ifneq ($(VEC),0)
CPUOPT := $(CPUOPT),zvfhmin=true,zvbb=true,zvkb=true,zvkt=true
endif
endif
QEMUOPTS = -machine virt,$(AIAOPT) $(CPUOPT) -bios none -kernel $K/kernel -m $(MEM) -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
# paging mode of the host kernel: 48 (default, needs Sv48 hardware; direct-maps
# up to 512 GiB) or 39 (three levels, RAM capped at 254 GiB). the word "sv39"
# on the host's command line selects the latter.
SV ?= 48
ifeq ($(SV),39)
QEMUOPTS += -append sv39
endif
FSIMG ?= fs.img
QEMUOPTS += -drive file=$(FSIMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
# networking: qemu's user-mode NAT. nothing is forwarded in (no hostfwd),
# no file sharing (no smb/tftp): the emulated network can only open
# outbound connections, and the kernel's egress policy limits those to
# public addresses (plus DHCP and the DNS forwarder). NET=0 removes it.
NET ?= 1
ifneq ($(NET),0)
QEMUOPTS += -netdev user,id=n0 -device virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1
endif

qemu: check-qemu-version $K/kernel fs.img
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $K/kernel .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

print-gdbport:
	@echo $(GDBPORT)

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')
check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi

.PHONY: fmt
fmt:
	clang-format -i $(wildcard kernel/*.[ch] user/*.[ch] mkfs/*.c)

# print the qemu command line (used by test-hyp.py)
# the command line only (for test-hyp.py, which builds the images itself
# with its own HOSTFS_MB/HOSTFILES; a dependency on fs.img here would
# rebuild it with the defaults).
qemucmd:
	@echo $(QEMU) $(QEMUOPTS)
