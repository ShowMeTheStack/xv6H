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
  $K/hypsbi.o

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

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB) $U/user.ld
	$(LD) $(LDFLAGS) -T $U/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

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
GUESTFS_BLOCKS ?= 4000
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

# a hostile test guest (see guest/badguest.c), used by test-hyp.py.
guest/badguest: guest/badguest.c guest/badentry.S guest/badguest.ld
	$(CC) -Wall -Werror -O -march=rv64g -mcmodel=medany -ffreestanding -fno-common -nostdlib \
	  -fno-stack-protector -fno-pie -no-pie -Wl,-T,guest/badguest.ld -o guest/badguest \
	  guest/badentry.S guest/badguest.c

fs.img: mkfs/mkfs README $(UPROGS) guest/kernel guest/badguest guest/hello guest/hello.bin $(GUESTDISKIMGS) $(HOSTFILES)
	mkfs/mkfs -s $$(( $(HOSTFS_MB) * 1024 )) fs.img README guest/kernel guest/badguest guest/hello guest/hello.bin $(GUESTDISKIMGS) $(UPROGS) $(HOSTFILES)

-include kernel/*.d user/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$K/kernel fs.img test-fs.img guest/kernel guest/badguest guest/hello guest/hello.bin guest/disk*.img \
	mkfs/mkfs .gdbinit \
        $U/usys.S \
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
HYP ?= 1
ifeq ($(HYP),0)
CPUOPT = -cpu rv64,h=false
else
CPUOPT = -cpu rv64
endif
QEMUOPTS = -machine virt,$(AIAOPT) $(CPUOPT) -bios none -kernel $K/kernel -m $(MEM) -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
FSIMG ?= fs.img
QEMUOPTS += -drive file=$(FSIMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

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
qemucmd: $K/kernel fs.img
	@echo $(QEMU) $(QEMUOPTS)
