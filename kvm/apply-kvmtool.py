#!/usr/bin/env python3
# Integrate the revise files (copied in beside this script by the deploy step) into
# ~/xv6h-kvm/kvmtool. Idempotent; `git diff` in kvmtool is the resulting patch.
import os, shutil, subprocess
T = os.path.expanduser("~/xv6h-kvm/kvmtool")
S = os.path.expanduser("~/xv6h-kvm/kvmtool-src")
# from the pristine tree every time: a patch whose text changed is applied afresh, not twice
subprocess.run(["git", "-C", T, "checkout", "--", "."], check=True)
for rel in ["riscv/revise.c", "riscv/revise-facts.c", "riscv/include/kvm/revise.h",
            "builtin-revise.c", "include/kvm/builtin-revise.h",
            "riscv/migrate.c", "riscv/include/kvm/migrate.h", "builtin-migrate.c", "include/kvm/builtin-migrate.h"]:
    os.makedirs(os.path.dirname(os.path.join(T, rel)), exist_ok=True)
    shutil.copy(os.path.join(S, rel), os.path.join(T, rel))
def sub(rel, pairs):
    p = os.path.join(T, rel); s = open(p).read(); o = s
    for a, b in pairs:
        if b in s: continue
        n = s.count(a); assert n == 1, (rel, a[:70], n)
        s = s.replace(a, b)
    if s != o: open(p, "w").write(s); print("patched", rel)

sub("Makefile", [("\tOBJS\t\t+= riscv/aia.o\n", "\tOBJS\t\t+= riscv/aia.o\n\tOBJS\t\t+= riscv/revise.o\n\tOBJS\t\t+= riscv/revise-facts.o\n"),
                 ("OBJS\t+= builtin-stop.o\n", "OBJS\t+= builtin-stop.o\nOBJS\t+= builtin-revise.o\n")])
sub("kvm-cmd.c", [('#include "kvm/builtin-stop.h"\n', '#include "kvm/builtin-stop.h"\n#include "kvm/builtin-revise.h"\n'),
                  ('\t{ "stop",\tkvm_cmd_stop,\t\tkvm_stop_help,\t\t0 },\n',
                   '\t{ "stop",\tkvm_cmd_stop,\t\tkvm_stop_help,\t\t0 },\n\t{ "revise",\tkvm_cmd_revise,\t\tkvm_revise_help,\t0 },\n')])
sub("include/kvm/kvm-ipc.h", [("\tKVM_IPC_VMSTATE\t= 8,\n", "\tKVM_IPC_VMSTATE\t= 8,\n\tKVM_IPC_REVISE\t= 9,\n")])
# the UAPI copy kvmtool builds against: the extension KVM forwards
sub("riscv/include/asm/kvm.h", [("\tKVM_RISCV_SBI_EXT_FWFT,\n", "\tKVM_RISCV_SBI_EXT_FWFT,\n\tKVM_RISCV_SBI_EXT_MACH,\n")])
# --revise
sub("riscv/include/kvm/kvm-config-arch.h", [
 ("\tbool\t\tsbi_ext_disabled[KVM_RISCV_SBI_EXT_MAX];\n", "\tbool\t\tsbi_ext_disabled[KVM_RISCV_SBI_EXT_MAX];\n\tbool\t\trevise;\n"),
 ("\tOPT_STRING('\\0', \"dump-dtb\", &(cfg)->dump_dtb_filename,\t\t\\\n",
  "\tOPT_BOOLEAN('\\0', \"revise\", &(cfg)->revise,\t\t\t\\\n\t\t    \"Revisable machine description (the MACH protocol and the anchor)\"),\\\n\tOPT_STRING('\\0', \"dump-dtb\", &(cfg)->dump_dtb_filename,\t\t\\\n")])
# the SBI exit: MACH goes to revise.c
sub("riscv/kvm-cpu.c", [('#include "kvm/kvm.h"\n', '#include "kvm/kvm.h"\n#include "kvm/revise.h"\n'),
 ("\tcase SBI_EXT_DBCN:\n\t\tswitch (vcpu->kvm_run->riscv_sbi.function_id) {",
  "\tcase 0x4D414348:\t/* MACH: revisable machine descriptions */\n\t\treturn revise__sbi(vcpu);\n\tcase SBI_EXT_DBCN:\n\t\tswitch (vcpu->kvm_run->riscv_sbi.function_id) {"),
 ("\tif (!kvm->cfg.arch.sbi_ext_disabled[KVM_RISCV_SBI_EXT_SUSP]) {",
  "\tif (kvm->cfg.arch.revise) {\n\t\tid = 1;\n\t\treg.id = RISCV_SBI_EXT_REG(KVM_REG_RISCV_SBI_SINGLE,\n\t\t\t\t\t   KVM_RISCV_SBI_EXT_MACH);\n\t\treg.addr = (unsigned long)&id;\n\t\tif (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)\n\t\t\tdie(\"KVM_SET_ONE_REG failed (sbi_ext MACH): this host's KVM does not forward MACH\");\n\t}\n\n\tif (!kvm->cfg.arch.sbi_ext_disabled[KVM_RISCV_SBI_EXT_SUSP]) {")])
# the FDT: built by a function that can write any slot; the anchor and the reservation in it
sub("riscv/fdt.c", [('#include "kvm/kvm-cpu.h"\n', '#include "kvm/kvm-cpu.h"\n#include "kvm/revise.h"\n'),
 ("static int setup_fdt(struct kvm *kvm)\n{", "int riscv__fdt_build(struct kvm *kvm, u64 dest_gpa)\n{"),
 ("\tvoid *fdt_dest\t\t= guest_flat_to_host(kvm,\n\t\t\t\t\t\t     kvm->arch.dtb_guest_start);",
  "\tvoid *fdt_dest\t\t= guest_flat_to_host(kvm, dest_gpa);"),
 ("\t/* CPUs */\n\tgenerate_cpu_nodes(fdt, kvm);", "\t/* the description slots (revisable machine descriptions) */\n\trevise__generate_reserved_memory(fdt, kvm);\n\n\t/* CPUs */\n\tgenerate_cpu_nodes(fdt, kvm);"),
 ("\t/* PCI host controller */\n\tpci__generate_fdt_nodes(fdt);", "\t/* the anchor */\n\trevise__generate_fdt_nodes(fdt, kvm, riscv__generate_irq_prop);\n\n\t/* PCI host controller */\n\tpci__generate_fdt_nodes(fdt);"),
 # packed in this process's memory and copied out whole: fdt_pack on the slot itself parsed
 # guest RAM that a running vcpu can write
 ("\t_FDT(fdt_open_into(fdt, fdt_dest, FDT_MAX_SIZE));\n\t_FDT(fdt_pack(fdt_dest));",
  "\t{\n\t\tvoid *packed = malloc(FDT_MAX_SIZE);\n\n\t\tif (!packed)\n\t\t\tdie(\"fdt: no memory to pack the tree\");\n\t\t_FDT(fdt_open_into(fdt, packed, FDT_MAX_SIZE));\n\t\t_FDT(fdt_pack(packed));\n\t\tmemcpy(fdt_dest, packed, fdt_totalsize(packed));\n\t\tfree(packed);\n\t}"),
 ("\tif (kvm->cfg.arch.dump_dtb_filename)\n\t\tdump_fdt(kvm->cfg.arch.dump_dtb_filename, fdt_dest);\n\treturn 0;\n}\nlate_init(setup_fdt);",
  "\tif (kvm->cfg.arch.dump_dtb_filename && dest_gpa == kvm->arch.dtb_guest_start)\n\t\tdump_fdt(kvm->cfg.arch.dump_dtb_filename, fdt_dest);\n\treturn 0;\n}\n\nstatic int setup_fdt(struct kvm *kvm)\n{\n\treturn riscv__fdt_build(kvm, kvm->arch.dtb_guest_start);\n}\nlate_init(setup_fdt);")])
# the CPU nodes come from a per-vcpu record taken at the first build (boot, before any vcpu
# runs): KVM_GET_ONE_REG on a vcpu waits for that vcpu to leave KVM_RUN, so a description
# rebuilt at run time cannot ask KVM again (it would hang). Facts edit the
# record (vector unit, cache block, hart IDs) instead.
sub("riscv/fdt.c", [
 ("#define CPU_NAME_MAX_LEN 15\nstatic void generate_cpu_nodes(void *fdt, struct kvm *kvm)\n{",
  "#define CPU_NAME_MAX_LEN 15\nstruct revise_cpu_desc revise_cpu[KVM_NR_CPUS];\nint revise_cpu_cached;\nstatic void generate_cpu_nodes(void *fdt, struct kvm *kvm)\n{"),
 ("\tfor (cpu = 0; cpu < kvm->nrcpus; ++cpu) {\n\t\tchar cpu_name[CPU_NAME_MAX_LEN];",
  "\tfor (cpu = 0; cpu < kvm->nrcpus; ++cpu) {\n\t\tif (revise_cpu_cached) {\n\t\t\trevise__generate_cpu_node(fdt, kvm, cpu);\n\t\t\tcontinue;\n\t\t}\n\t\t{\n\t\tchar cpu_name[CPU_NAME_MAX_LEN];"),
 ("\t\t_FDT(fdt_property_cell(fdt, \"reg\", cpu));\n\t\t_FDT(fdt_property_string(fdt, \"status\", \"okay\"));",
  "\t\t_FDT(fdt_property_cell(fdt, \"reg\", cpu));\n\t\t_FDT(fdt_property_string(fdt, \"status\", \"okay\"));\n\t\trevise__record_cpu(cpu, cpu_isa, cbom_blksz, cboz_blksz, cbop_blksz, satp_mode, vcpu->riscv_xlen);"),
 ("\t\t_FDT(fdt_end_node(fdt));\n\n\t\t_FDT(fdt_end_node(fdt));\n\t}\n\n\t_FDT(fdt_end_node(fdt));\n}",
  "\t\t_FDT(fdt_end_node(fdt));\n\n\t\t_FDT(fdt_end_node(fdt));\n\t\t}\n\t}\n\trevise_cpu_cached = 1;\n\n\t_FDT(fdt_end_node(fdt));\n}"),
])
print("cpu record integrated")
# ---- F15 (device map) and F3 (interrupt numbering) for virtio-mmio ----
# ioeventfd: the same eventfd at a second address while the old one still answers, then the
# old address dropped at the ack (vhost polls the eventfd in the kernel, so it must be the same fd)
sub("include/kvm/ioeventfd.h", [("int ioeventfd__del_event(u64 addr, u64 datamatch);",
 "int ioeventfd__del_event(u64 addr, u64 datamatch);\nint ioeventfd__alias_event(u64 addr, u64 datamatch, u64 new_addr);\nint ioeventfd__move_event(u64 old_addr, u64 datamatch, u64 new_addr);")])
sub("ioeventfd.c", [("int ioeventfd__del_event(u64 addr, u64 datamatch)\n{",
 """static struct ioevent *ioevent_find(u64 addr, u64 datamatch)
{
	struct ioevent *ioevent;

	list_for_each_entry(ioevent, &used_ioevents, list)
		if (ioevent->io_addr == addr && ioevent->datamatch == datamatch)
			return ioevent;
	return NULL;
}

/* revisable device map: the event's eventfd also at new_addr (the alias window) */
int ioeventfd__alias_event(u64 addr, u64 datamatch, u64 new_addr)
{
	struct ioevent *e = ioevent_find(addr, datamatch);
	struct kvm_ioeventfd k;

	if (!e)
		return -ENOENT;
	k = (struct kvm_ioeventfd) { .addr = new_addr, .len = e->io_len, .datamatch = e->datamatch,
				     .fd = e->fd, .flags = KVM_IOEVENTFD_FLAG_DATAMATCH };
	return ioctl(e->fn_kvm->vm_fd, KVM_IOEVENTFD, &k) ? -errno : 0;
}

/* ... and at the ack the old address goes; the event is known by its new one */
int ioeventfd__move_event(u64 old_addr, u64 datamatch, u64 new_addr)
{
	struct ioevent *e = ioevent_find(old_addr, datamatch);
	struct kvm_ioeventfd k;

	if (!e)
		return -ENOENT;
	k = (struct kvm_ioeventfd) { .addr = old_addr, .len = e->io_len, .datamatch = e->datamatch,
				     .fd = e->fd, .flags = KVM_IOEVENTFD_FLAG_DATAMATCH | KVM_IOEVENTFD_FLAG_DEASSIGN };
	ioctl(e->fn_kvm->vm_fd, KVM_IOEVENTFD, &k);
	e->io_addr = new_addr;
	return 0;
}

int ioeventfd__del_event(u64 addr, u64 datamatch)
{""")])
# virtio-mmio: the old window and the old line, kept until the ack
sub("include/kvm/virtio-mmio.h", [("struct virtio_mmio {\n\tu32\t\t\taddr;\n\tvoid\t\t\t*dev;\n\tstruct kvm\t\t*kvm;\n\tu8\t\t\tirq;\n",
 "struct virtio_mmio {\n\tu32\t\t\taddr;\n\tu32\t\t\told_addr;\t/* F15: the window the guest may still use (0: none) */\n\tvoid\t\t\t*dev;\n\tstruct kvm\t\t*kvm;\n\tu8\t\t\tirq;\t\t/* the line it raises now */\n\tu8\t\t\tirq_desc;\t/* F3: the line the description gives it */\n\tu8\t\t\tirq_held;\t/* F3: quiet window, an edge owed at the ack */\n\tbool\t\t\tirq_quiet;\n\tvoid\t\t\t*mmio_fn;\n\tvoid\t\t\t*vdev;\n\tstruct list_head\trevise_list;\n"),
 ("int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev);",
  "int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev);\n/* revisable machine descriptions */\nint virtio_mmio_relocate_all(struct kvm *kvm, long delta);\nvoid virtio_mmio_relocate_end(struct kvm *kvm);\nvoid virtio_mmio_relocate_cancel(struct kvm *kvm);\nvoid virtio_mmio_renumber_cancel(struct kvm *kvm);\nint virtio_mmio_renumber_all(struct kvm *kvm, const u8 *lines, int n);\nvoid virtio_mmio_quiet_all(struct kvm *kvm, bool on);\nvoid virtio_mmio_renumber_end(struct kvm *kvm);\nint virtio_mmio_count(void);\nint virtio_mmio_lines(u8 *out, int n);")])
for f in ["virtio/mmio-modern.c", "virtio/mmio-legacy.c"]:
    sub(f, [("\tu32 offset = addr - vmmio->addr;",
             "\tu32 offset = addr - ((vmmio->old_addr && addr >= vmmio->old_addr &&\n\t\t\t      addr < vmmio->old_addr + VIRTIO_MMIO_IO_SIZE) ? vmmio->old_addr : vmmio->addr);")])
sub("virtio/mmio.c", [
 ("static u32 virtio_mmio_io_space_blocks = KVM_VIRTIO_MMIO_AREA;",
  "static u32 virtio_mmio_io_space_blocks = KVM_VIRTIO_MMIO_AREA;\nstatic LIST_HEAD(revise_vmmios);\t/* every virtio-mmio device, for the facts */"),
 ("\tvmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;\n\tkvm__irq_trigger(vmmio->kvm, vmmio->irq);",
  "\tvmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;\n\tif (vmmio->irq_quiet) { vmmio->irq_held = 1; return 0; }\t/* F3: owed at the ack */\n\tkvm__irq_trigger(vmmio->kvm, vmmio->irq);"),
 ("\tvmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;\n\tkvm__irq_trigger(vmmio->kvm, vmmio->irq);",
  "\tvmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;\n\tif (vmmio->irq_quiet) { vmmio->irq_held = 1; return 0; }\n\tkvm__irq_trigger(vmmio->kvm, vmmio->irq);"),
 ("\tgenerate_irq_prop(fdt, vmmio->irq, IRQ_TYPE_EDGE_RISING);", "\tgenerate_irq_prop(fdt, vmmio->irq_desc, IRQ_TYPE_EDGE_RISING);"),
 ("\tvmmio->irq = irq__alloc_line();\n",
  "\tvmmio->irq = irq__alloc_line();\n\tvmmio->irq_desc = vmmio->irq;\n\tvmmio->mmio_fn = legacy ? (void *)virtio_mmio_legacy_callback : (void *)virtio_mmio_modern_callback;\n\tvmmio->vdev = vdev;\n\tlist_add_tail(&vmmio->revise_list, &revise_vmmios);\n"),
 ("int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)\n{",
  """typedef void (*mmio_cb_t)(struct kvm_cpu *, u64, u8 *, u32, u8, void *);

int virtio_mmio_count(void)
{
	struct virtio_mmio *v;
	int n = 0;

	list_for_each_entry(v, &revise_vmmios, revise_list)
		n++;
	return n;
}

int virtio_mmio_lines(u8 *out, int n)
{
	struct virtio_mmio *v;
	int i = 0;

	list_for_each_entry(v, &revise_vmmios, revise_list)
		if (i < n)
			out[i++] = v->irq_desc;
	return i;
}

/*
 * F15: every virtio-mmio window moves by delta. The new window and the notify eventfds at
 * the new address are added; the old ones keep answering until the guest acknowledges.
 */
int virtio_mmio_relocate_all(struct kvm *kvm, long delta)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		u32 to = v->addr + delta;
		struct virtio_device *vdev = v->vdev;

		if (v->old_addr)
			return -EBUSY;		/* a move still owed */
		if (kvm__register_mmio(kvm, to, VIRTIO_MMIO_IO_SIZE, false, (mmio_cb_t)v->mmio_fn, vdev) < 0)
			return -EIO;
		for (u32 vq = 0; vq < VIRTIO_MMIO_MAX_VQ; vq++)
			if (v->ioeventfds[vq].vdev)
				ioeventfd__alias_event(v->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq, to + VIRTIO_MMIO_QUEUE_NOTIFY);
		fprintf(stderr, "revise: virtio-mmio window 0x%x -> 0x%x (line %u)\\n", v->addr, to, v->irq_desc);
		v->old_addr = v->addr;
		v->addr = to;
	}
	return 0;
}

/* at the ack: the old windows answer their last */
void virtio_mmio_relocate_end(struct kvm *kvm)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		if (!v->old_addr)
			continue;
		for (u32 vq = 0; vq < VIRTIO_MMIO_MAX_VQ; vq++)
			if (v->ioeventfds[vq].vdev)
				ioeventfd__move_event(v->old_addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq, v->addr + VIRTIO_MMIO_QUEUE_NOTIFY);
		kvm__deregister_mmio(kvm, v->old_addr);
		v->old_addr = 0;
	}
}

/*
 * The guest did not take the device revision (its ack had no REV_DEVICES): the move is
 * cancelled, not completed. Ending the alias anyway took the windows away from a guest still
 * using them (its disk stopped answering: the KVM port's first vic tree, every move "not moved").
 */
void virtio_mmio_relocate_cancel(struct kvm *kvm)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		if (!v->old_addr)
			continue;
		for (u32 vq = 0; vq < VIRTIO_MMIO_MAX_VQ; vq++)
			if (v->ioeventfds[vq].vdev)
				ioeventfd__move_event(v->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq, v->old_addr + VIRTIO_MMIO_QUEUE_NOTIFY);
		kvm__deregister_mmio(kvm, v->addr);
		v->addr = v->old_addr;
		v->old_addr = 0;
	}
}

/* ... and a renumbering not taken: every device keeps the line the guest knows */
void virtio_mmio_renumber_cancel(struct kvm *kvm)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		v->irq_desc = v->irq;
		v->irq_quiet = false;
		if (v->irq_held) {
			v->irq_held = 0;
			kvm__irq_trigger(kvm, v->irq);
		}
	}
}

/* F3: the description gives device i line lines[i]; the device raises its old line until the ack */
int virtio_mmio_renumber_all(struct kvm *kvm, const u8 *lines, int n)
{
	struct virtio_mmio *v;
	int i = 0;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		if (i >= n)
			return -EINVAL;
		v->irq_desc = lines[i++];
	}
	return 0;
}

/* F3's quiet window: from the guest's describe() to its ack, edges are held, not raised */
void virtio_mmio_quiet_all(struct kvm *kvm, bool on)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list)
		if (v->irq_desc != v->irq)
			v->irq_quiet = on;
}

/* at the ack: every device raises its described line, and an edge held is raised there */
void virtio_mmio_renumber_end(struct kvm *kvm)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		v->irq = v->irq_desc;
		v->irq_quiet = false;
		if (v->irq_held || v->hdr.interrupt_state) {
			v->irq_held = 0;
			kvm__irq_trigger(kvm, v->irq);
		}
	}
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{"""),
])
print("A.2 integrated")
# harness: the console's input thread also on a FIFO (not only on a terminal), so a test can
# type into a guest that runs in the background
sub("term.c", [("\t\tkvm__arch_read_term(kvm);\n\t}\n\n\tdie(",
 "\t\tkvm__arch_read_term(kvm);\n\t\t/* the 8250 takes input only into an empty FIFO: while the guest drains it, a\n\t\t   readable input would spin this thread on the device mutex and starve the vcpu */\n\t\tusleep(1000);\n\t}\n\n\tdie("), ("\tif (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))\n\t\treturn 0;\n",
 "\tif (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {\n\t\tif (pthread_create(&term_poll_thread, NULL, term_poll_thread_loop, kvm))\n\t\t\tdie(\"Unable to create console input poll thread\\n\");\n\t\treturn 0;\n\t}\n")])
# harness (an upstream bug): every 8250 port reads the same stdin unless given its own tty, so
# once ttyS0's FIFO is full the next bytes go to ttyS1..3, which nothing reads, and a typed line
# longer than 64 bytes loses its tail. A port whose input is the first port's leaves it alone.
sub("include/kvm/term.h", [("bool term_readable(int term);", "bool term_readable(int term);\nbool term_shares_input(int term);")])
sub("term.c", [("bool term_readable(int term)\n{", "bool term_shares_input(int term)\n{\n\treturn term && term_fds[term][TERM_FD_IN] == term_fds[0][TERM_FD_IN];\n}\n\nbool term_readable(int term)\n{")])
sub("hw/serial.c", [("\tif (kvm->cfg.active_console != CONSOLE_8250)\n\t\treturn;\n\n\twhile (term_readable(dev->id)",
 "\tif (kvm->cfg.active_console != CONSOLE_8250)\n\t\treturn;\n\n\tif (term_shares_input(dev->id))\n\t\treturn;\n\n\twhile (term_readable(dev->id)")])
# ---- F1, the controller's kind, between kvmtool's PLIC and KVM's in-kernel AIA ----
# Both are resident from boot (nothing is created or torn down under a running VM); the
# description names one through an xv6h,vic node, the lines are raised on the one described
# from the ack on, and every line still owed is raised again there (Theorem 1).
p = os.path.join(T, "riscv/plic.c"); s = open(p).read()
if "plic_resident" not in s:
    s = s.replace("static struct plic_state plic;\n",
        "static struct plic_state plic;\n"
        "/* xv6H (F1): resident beside the in-kernel AIA, at a window of its own; else kvmtool's PLIC */\n"
        "static bool plic_resident;\nstatic u64 plic_base = RISCV_IRQCHIP, plic_size = RISCV_IRQCHIP_SIZE;\n", 1)
    s = s.replace("RISCV_IRQCHIP_SIZE)", "plic_size)").replace("RISCV_IRQCHIP_SIZE,", "plic_size,")
    s = s.replace("addr -= RISCV_IRQCHIP;", "addr -= plic_base;")
    s = s.replace("reg_cells[1] = cpu_to_fdt32(RISCV_IRQCHIP);", "reg_cells[1] = cpu_to_fdt32(plic_base);")
    s = s.replace('(u32)RISCV_IRQCHIP);', '(u32)plic_base);')
    s = s.replace("kvm__register_mmio(kvm, RISCV_IRQCHIP, ", "kvm__register_mmio(kvm, plic_base, ")
    s = s.replace("kvm__deregister_mmio(kvm, RISCV_IRQCHIP);", "kvm__deregister_mmio(kvm, plic_base);")
    s = s.replace("static void plic__irq_trig(struct kvm *kvm, int irq, int level, bool edge)",
                  "void plic__irq_trig(struct kvm *kvm, int irq, int level, bool edge)")
    s = s.replace("\tif (riscv_irqchip != IRQCHIP_PLIC)\n\t\treturn 0;\n\n\tplic.kvm = kvm;",
        "\tif (riscv_irqchip != IRQCHIP_PLIC && !kvm->cfg.arch.revise)\n\t\treturn 0;\n"
        "\tif (riscv_irqchip != IRQCHIP_PLIC) {\n\t\tplic_resident = true;\n"
        "\t\tplic_base = 0x0c000000ULL;\t/* inside the irqchip window, clear of the AIA's pages */\n"
        "\t\tplic_size = 0x04000000ULL;\n\t}\n\n\tplic.kvm = kvm;", 1)
    s = s.replace("\t/* Setup default IRQ routing */\n\tplic__irq_routing_init(kvm);\n\n\tplic.ready = true;",
        "\t/* Setup default IRQ routing (the AIA's, when resident beside it) */\n\tif (!plic_resident)\n\t\tplic__irq_routing_init(kvm);\n\n\tplic.ready = true;", 1)
    s = s.replace("static int plic__exit(struct kvm *kvm)\n{\n\tif (riscv_irqchip != IRQCHIP_PLIC)\n",
                  "static int plic__exit(struct kvm *kvm)\n{\n\tif (riscv_irqchip != IRQCHIP_PLIC && !plic_resident)\n", 1)
    # an edge is not a held level: latched in irq_level it is never cleared (a claim clears only
    # the pending bit), and the next enable of the line (a guest's mask/unmask, or a controller
    # taken again) makes a pending interrupt nothing can acknowledge: an interrupt storm
    s = s.replace("\tif (level)\n\t\ts->irq_level[irq_word] |= irq_mask;\n\telse\n\t\ts->irq_level[irq_word] &= ~irq_mask;",
                  "\tif (level && !edge)\n\t\ts->irq_level[irq_word] |= irq_mask;\n\telse\n\t\ts->irq_level[irq_word] &= ~irq_mask;", 1)
    assert "if (level && !edge)" in s, "plic edge latch"
    s = s.replace("static int plic__irq_routing_init(struct kvm *kvm)\n{",
        "/*\n * xv6H (F1): the PLIC as a controller the guest is about to take again starts clean. A line the\n"
        " * guest claimed and completed on another backend after it left this one would stay claimed here\n"
        " * and never be signalled again; whatever is owed is raised again after this (Theorem 1).\n */\n"
        "void plic__reset_state(struct kvm *kvm)\n{\n\tstruct plic_state *s = &plic;\n\tu32 i;\n\n"
        "\tif (!s->ready)\n\t\treturn;\n\tmutex_lock(&s->irq_lock);\n"
        "\tfor (i = 0; i < s->num_context; i++) {\n\t\tstruct plic_context *c = &s->contexts[i];\n\n"
        "\t\tmutex_lock(&c->irq_lock);\n"
        "\t\tmemset(c->irq_pending, 0, sizeof(c->irq_pending));\n\t\tmemset(c->irq_pending_priority, 0, sizeof(c->irq_pending_priority));\n"
        "\t\tmemset(c->irq_claimed, 0, sizeof(c->irq_claimed));\n\t\tmemset(c->irq_autoclear, 0, sizeof(c->irq_autoclear));\n"
        "\t\t__plic_context_irq_update(s, c);\n\t\tmutex_unlock(&c->irq_lock);\n\t}\n"
        "\tmemset(s->irq_level, 0, sizeof(s->irq_level));\n\tmutex_unlock(&s->irq_lock);\n}\n\n"
        "static int plic__irq_routing_init(struct kvm *kvm)\n{", 1)
    for k in ["plic_resident = true", "addr -= plic_base", "void plic__irq_trig", "kvm__register_mmio(kvm, plic_base", "!plic_resident)\n\t\tplic__irq"]:
        assert k in s, k
    assert "RISCV_IRQCHIP" not in s.replace("plic_base = RISCV_IRQCHIP, plic_size = RISCV_IRQCHIP_SIZE", ""), "plic.c still names RISCV_IRQCHIP"
    open(p, "w").write(s); print("patched riscv/plic.c")
sub("riscv/include/kvm/kvm-arch.h", [("extern void (*riscv_irqchip_generate_fdt_node)(void *fdt, struct kvm *kvm);",
 "extern void (*riscv_irqchip_generate_fdt_node)(void *fdt, struct kvm *kvm);\n"
 "/* xv6H: the lines raised on the controller the description names (F1), and their last levels */\n"
 "extern void (*riscv_irqchip_route)(struct kvm *kvm, int irq, int level, bool edge);\n"
 "extern u8 riscv_irq_level[256];\n"
 "void plic__irq_trig(struct kvm *kvm, int irq, int level, bool edge);\n"
 "void plic__reset_state(struct kvm *kvm);")])
sub("riscv/irq.c", [
 ("void (*riscv_irqchip_generate_fdt_node)(void *fdt, struct kvm *kvm) = NULL;\n",
  "void (*riscv_irqchip_generate_fdt_node)(void *fdt, struct kvm *kvm) = NULL;\n"
  "void (*riscv_irqchip_route)(struct kvm *kvm, int irq, int level, bool edge) = NULL;\n"
  "u8 riscv_irq_level[256];\n"),
 ("void kvm__irq_line(struct kvm *kvm, int irq, int level)\n{\n\tstruct kvm_irq_level irq_level;\n\n",
  "void kvm__irq_line(struct kvm *kvm, int irq, int level)\n{\n\tstruct kvm_irq_level irq_level;\n\n"
  "\tif (irq >= 0 && irq < 256)\n\t\triscv_irq_level[irq] = !!level;\t/* what a new controller is owed (F1) */\n"
  "\tif (riscv_irqchip_route) {\n\t\triscv_irqchip_route(kvm, irq, level, false);\n\t\treturn;\n\t}\n"),
 ("void kvm__irq_trigger(struct kvm *kvm, int irq)\n{\n",
  "void kvm__irq_trigger(struct kvm *kvm, int irq)\n{\n\tif (riscv_irqchip_route) {\n\t\triscv_irqchip_route(kvm, irq, 1, true);\n\t\treturn;\n\t}\n")])
sub("include/kvm/virtio-mmio.h", [("int virtio_mmio_count(void);", "int virtio_mmio_count(void);\nint virtio_mmio_reraise_all(struct kvm *kvm);")])
sub("virtio/mmio.c", [("int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)\n{",
 """/* F1, Theorem 1: a device whose status is not acknowledged is owed its line on a new controller */
int virtio_mmio_reraise_all(struct kvm *kvm)
{
	struct virtio_mmio *v;
	int n = 0;

	list_for_each_entry(v, &revise_vmmios, revise_list)
		if (v->hdr.interrupt_state) {
			kvm__irq_trigger(kvm, v->irq);
			n++;
		}
	return n;
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{""")])
# ---- Migration (riscv/migrate.c): each device's state out and in ----
sub("Makefile", [("\tOBJS\t\t+= riscv/revise-facts.o\n", "\tOBJS\t\t+= riscv/revise-facts.o\n\tOBJS\t\t+= riscv/migrate.o\n"),
                 ("OBJS\t+= builtin-revise.o\n", "OBJS\t+= builtin-revise.o\nOBJS\t+= builtin-migrate.o\n")])
sub("include/kvm/kvm-ipc.h", [("\tKVM_IPC_REVISE\t= 9,\n", "\tKVM_IPC_REVISE\t= 9,\n\tKVM_IPC_MIGRATE\t= 10,\n")])
sub("kvm-cmd.c", [('#include "kvm/builtin-revise.h"\n', '#include "kvm/builtin-revise.h"\n#include "kvm/builtin-migrate.h"\n'),
 ('\t{ "revise",\tkvm_cmd_revise,\t\tkvm_revise_help,\t0 },\n',
  '\t{ "revise",\tkvm_cmd_revise,\t\tkvm_revise_help,\t0 },\n\t{ "migrate",\tkvm_cmd_migrate,\tkvm_migrate_help,\t0 },\n')])
# --incoming PORT: the state arrives before the vcpus first run
sub("riscv/include/kvm/kvm-config-arch.h", [("\tbool\t\trevise;\n", "\tbool\t\trevise;\n\tint\t\tincoming;\n"),
 ("\tOPT_BOOLEAN('\\0', \"revise\", &(cfg)->revise,",
  "\tOPT_INTEGER('\\0', \"incoming\", &(cfg)->incoming,\t\t\t\\\n\t\t    \"Receive a migrating machine on this TCP port before running\"),\\\n\tOPT_BOOLEAN('\\0', \"revise\", &(cfg)->revise,")])
sub("builtin-run.c", [('#include "kvm/builtin-run.h"\n', '#include "kvm/builtin-run.h"\n#include "kvm/migrate.h"\n'),
 ("static int kvm_cmd_run_work(struct kvm *kvm)\n{\n\tvoid *vcpu0_ret;\n\tint i;\n\n",
  "static int kvm_cmd_run_work(struct kvm *kvm)\n{\n\tvoid *vcpu0_ret;\n\tint i;\n\n\tif (kvm->cfg.arch.incoming)\n\t\tmigrate__incoming(kvm, kvm->cfg.arch.incoming);\t/* A.6 */\n\n")])
# AIA: the device and its sizes
p = os.path.join(T, "riscv/aia.c"); t = open(p).read()
if "int aia__fd(void)" not in t:
    t = t.replace('#include "kvm/util.h"\n', '#include "kvm/util.h"\n#include "kvm/migrate.h"\n', 1)
    t += "\n/* A.6: migration reads and writes the in-kernel AIA's registers through this device */\nint aia__fd(void)\n{\n\treturn aia_fd;\n}\n\nint aia__nr_sources(void)\n{\n\treturn aia_nr_sources;\n}\n\nint aia__nr_ids(void)\n{\n\treturn aia_nr_ids;\n}\n"
    open(p, "w").write(t); print("patched riscv/aia.c")
# PLIC: the model's state
p = os.path.join(T, "riscv/plic.c"); t = open(p).read()
if "plic__state_send" not in t:
    t = t.replace('#include "kvm/irq.h"\n', '#include "kvm/irq.h"\n#include "kvm/migrate.h"\n', 1)
    t += r"""
/* A.6: the model's state, out and in (the contexts' enables, pending, claimed; priorities; levels) */
struct plic_ctx_state {
	u8 threshold;
	u32 enable[MAX_DEVICES / 32], pending[MAX_DEVICES / 32], claimed[MAX_DEVICES / 32], autoclear[MAX_DEVICES / 32];
	u8 pending_priority[MAX_DEVICES];
};
struct plic_mig {
	u32 num_context;
	u8 priority[MAX_DEVICES];
	u32 level[MAX_DEVICES / 32];
	struct plic_ctx_state c[];
};

int plic__state_send(int fd, mig_put_t put)
{
	struct plic_mig *m;
	size_t n;
	int r;

	if (!plic.ready)
		return 0;
	n = sizeof(*m) + plic.num_context * sizeof(struct plic_ctx_state);
	m = calloc(1, n);
	if (!m)
		return -1;
	m->num_context = plic.num_context;
	memcpy(m->priority, plic.irq_priority, sizeof(m->priority));
	memcpy(m->level, plic.irq_level, sizeof(m->level));
	for (u32 i = 0; i < plic.num_context; i++) {
		struct plic_context *c = &plic.contexts[i];

		m->c[i].threshold = c->irq_priority_threshold;
		memcpy(m->c[i].enable, c->irq_enable, sizeof(c->irq_enable));
		memcpy(m->c[i].pending, c->irq_pending, sizeof(c->irq_pending));
		memcpy(m->c[i].claimed, c->irq_claimed, sizeof(c->irq_claimed));
		memcpy(m->c[i].autoclear, c->irq_autoclear, sizeof(c->irq_autoclear));
		memcpy(m->c[i].pending_priority, c->irq_pending_priority, sizeof(c->irq_pending_priority));
	}
	r = put(fd, T_PLIC, m, n);
	free(m);
	return r;
}

void plic__state_apply(void *buf, u64 len)
{
	struct plic_mig *m = buf;

	if (!plic.ready)
		return;
	/* the contexts follow the header: all of them, or the record is not this machine's */
	if (len < sizeof(*m) || m->num_context != plic.num_context ||
	    len < sizeof(*m) + (u64)plic.num_context * sizeof(m->c[0]))
		die("migrate: a PLIC record that does not fit here");
	memcpy(plic.irq_priority, m->priority, sizeof(m->priority));
	memcpy(plic.irq_level, m->level, sizeof(m->level));
	for (u32 i = 0; i < plic.num_context; i++) {
		struct plic_context *c = &plic.contexts[i];

		c->irq_priority_threshold = m->c[i].threshold;
		memcpy(c->irq_enable, m->c[i].enable, sizeof(c->irq_enable));
		memcpy(c->irq_pending, m->c[i].pending, sizeof(c->irq_pending));
		memcpy(c->irq_claimed, m->c[i].claimed, sizeof(c->irq_claimed));
		memcpy(c->irq_autoclear, m->c[i].autoclear, sizeof(c->irq_autoclear));
		memcpy(c->irq_pending_priority, m->c[i].pending_priority, sizeof(c->irq_pending_priority));
		__plic_context_irq_update(&plic, c);
	}
}
"""
    open(p, "w").write(t); print("patched riscv/plic.c (state)")
# 8250: the registers and buffers
p = os.path.join(T, "hw/serial.c"); t = open(p).read()
if "serial8250__state_send" not in t:
    t = t.replace('#include "kvm/fdt.h"\n', '#include "kvm/fdt.h"\n#include "kvm/migrate.h"\n', 1)
    t += r"""
/* A.6: the 8250s' registers and FIFOs, out and in */
struct serial_mig {
	u8 irq_state, dll, dlm, iir, ier, fcr, lcr, mcr, lsr, msr, scr;
	int txcnt, rxcnt, rxdone;
	char txbuf[FIFO_LEN], rxbuf[FIFO_LEN];
};

int serial8250__state_send(int fd, mig_put_t put)
{
	struct serial_mig m[ARRAY_SIZE(devices)];

	for (unsigned i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *d = &devices[i];

		m[i] = (struct serial_mig){ d->irq_state, d->dll, d->dlm, d->iir, d->ier, d->fcr, d->lcr, d->mcr,
					    d->lsr, d->msr, d->scr, d->txcnt, d->rxcnt, d->rxdone };
		memcpy(m[i].txbuf, d->txbuf, FIFO_LEN);
		memcpy(m[i].rxbuf, d->rxbuf, FIFO_LEN);
	}
	return put(fd, T_SERIAL, m, sizeof(m));
}

void serial8250__state_apply(void *buf, u64 len)
{
	struct serial_mig *m = buf;

	if (len != sizeof(struct serial_mig) * ARRAY_SIZE(devices))
		return;
	for (unsigned i = 0; i < ARRAY_SIZE(devices); i++)
		/* the counts index the FIFOs: a count outside them is not a port's state */
		if (m[i].txcnt < 0 || m[i].txcnt > FIFO_LEN || m[i].rxcnt < 0 || m[i].rxcnt > FIFO_LEN ||
		    m[i].rxdone < 0 || m[i].rxdone > m[i].rxcnt)
			die("migrate: a serial record that does not fit here");
	for (unsigned i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *d = &devices[i];

		d->irq_state = m[i].irq_state; d->dll = m[i].dll; d->dlm = m[i].dlm; d->iir = m[i].iir;
		d->ier = m[i].ier; d->fcr = m[i].fcr; d->lcr = m[i].lcr; d->mcr = m[i].mcr; d->lsr = m[i].lsr;
		d->msr = m[i].msr; d->scr = m[i].scr; d->txcnt = m[i].txcnt; d->rxcnt = m[i].rxcnt;
		d->rxdone = m[i].rxdone;
		memcpy(d->txbuf, m[i].txbuf, FIFO_LEN);
		memcpy(d->rxbuf, m[i].rxbuf, FIFO_LEN);
	}
}
"""
    open(p, "w").write(t); print("patched hw/serial.c (state)")
# virtio-mmio: transport, features, status, rings, window and line
sub("virtio/mmio.c", [('#include "kvm/fdt.h"\n', '#include "kvm/fdt.h"\n#include "kvm/migrate.h"\n'),
 ("int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)\n{",
  """/* A.6: every virtio-mmio device, out and in, in the order the devices were made */
#define MIG_VQS 8
struct vq_mig {
	struct vring_addr vring_addr;
	u16 last_avail_idx, last_used_signalled;
	u8 enabled;
};
struct vmmio_mig {
	struct virtio_mmio_hdr hdr;
	u64 features;
	u32 status, addr, irq;
	struct vq_mig vq[MIG_VQS];
};

int virtio_mmio_state_send(struct kvm *kvm, int fd, mig_put_t put)
{
	struct virtio_mmio *v;
	int n = virtio_mmio_count(), i = 0, r;
	struct vmmio_mig *m = calloc(n ? n : 1, sizeof(*m));

	if (!m)
		return -1;
	list_for_each_entry(v, &revise_vmmios, revise_list) {
		struct virtio_device *vdev = v->vdev;
		unsigned nvq = vdev->ops->get_vq_count ? vdev->ops->get_vq_count(kvm, v->dev) : 0;

		m[i].hdr = v->hdr;
		m[i].features = vdev->features;
		m[i].status = vdev->status;
		m[i].addr = v->addr;
		m[i].irq = v->irq;
		for (unsigned q = 0; q < nvq && q < MIG_VQS; q++) {
			struct virt_queue *vq = vdev->ops->get_vq(kvm, v->dev, q);

			m[i].vq[q].vring_addr = vq->vring_addr;
			m[i].vq[q].last_avail_idx = vq->last_avail_idx;
			m[i].vq[q].last_used_signalled = vq->last_used_signalled;
			m[i].vq[q].enabled = vq->enabled;
		}
		i++;
	}
	r = put(fd, T_VIRTIO, m, n * sizeof(*m));
	free(m);
	return r;
}

void virtio_mmio_state_apply(struct kvm *kvm, void *buf, u64 len)
{
	struct vmmio_mig *m = buf;
	struct virtio_mmio *v;
	int n = len / sizeof(*m), i = 0;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		struct virtio_device *vdev = v->vdev;
		unsigned nvq;

		if (i >= n)
			break;
		/* a window is where this device is, or where F15 moves it: nowhere else */
		if (m[i].addr != v->addr && m[i].addr != v->addr + 0x100000 && m[i].addr != v->addr - 0x100000)
			die("migrate: a device window at 0x%x for a device at 0x%x", m[i].addr, v->addr);
		if (m[i].irq == 0 || m[i].irq >= 256)
			die("migrate: a device line %u", m[i].irq);
		if (m[i].addr != v->addr) {	/* the source had moved it (F15): here too */
			kvm__deregister_mmio(kvm, v->addr);
			v->addr = m[i].addr;
			if (kvm__register_mmio(kvm, v->addr, VIRTIO_MMIO_IO_SIZE, false,
					       (void (*)(struct kvm_cpu *, u64, u8 *, u32, u8, void *))v->mmio_fn, vdev) < 0)
				pr_warning("migrate: the device window at 0x%x could not be made here", v->addr);
		}
		v->irq = v->irq_desc = m[i].irq;	/* and renumbered it (F3) */
		v->hdr = m[i].hdr;
		vdev->features = m[i].features;
		vdev->status = m[i].status;
		nvq = vdev->ops->get_vq_count ? vdev->ops->get_vq_count(kvm, v->dev) : 0;
		/* the device as the guest left it: its configuration and, if the driver was running, started */
		if (vdev->ops->notify_status)
			vdev->ops->notify_status(kvm, v->dev, VIRTIO__STATUS_CONFIG |
						 ((vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ? VIRTIO__STATUS_START : 0));
		for (unsigned q = 0; q < nvq && q < MIG_VQS; q++) {
			struct virt_queue *vq = vdev->ops->get_vq(kvm, v->dev, q);

			if (!m[i].vq[q].enabled)
				continue;
			vq->vring_addr = m[i].vq[q].vring_addr;
			virtio_mmio_init_vq(kvm, vdev, q);	/* the rings, from the guest's addresses here */
			vq->last_avail_idx = m[i].vq[q].last_avail_idx;
			vq->last_used_signalled = m[i].vq[q].last_used_signalled;
		}
		i++;
	}
}

/* what the guest posted and the source had not taken is in the rings: the queues are looked at once */
void virtio_mmio_kick_all(struct kvm *kvm)
{
	struct virtio_mmio *v;

	list_for_each_entry(v, &revise_vmmios, revise_list) {
		struct virtio_device *vdev = v->vdev;
		unsigned nvq = vdev->ops->get_vq_count ? vdev->ops->get_vq_count(kvm, v->dev) : 0;

		for (unsigned q = 0; q < nvq && q < MIG_VQS; q++)
			if (vdev->ops->get_vq(kvm, v->dev, q)->enabled)
				vdev->ops->notify_vq(kvm, v->dev, q);
		if (v->hdr.interrupt_state)
			kvm__irq_trigger(kvm, v->irq);
	}
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{""")])
print("kvmtool integrated")
