/*
 * Revisable machine descriptions in kvmtool.
 *
 * The VMM owns the machine's description. A revision changes one or more facts of the
 * machine, writes the new device tree into the free description slot, raises the
 * generation, and announces it on the anchor (a page of registers and one interrupt line
 * that no revision moves). The guest reads the generation and the description through
 * the SBI MACH extension (0x4D414348, forwarded to us by KVM), takes what it can, and
 * acknowledges with the generation it read. Until then every old value keeps answering
 * (the alias invariant); the facts' own code keeps the alias.
 *
 * The protocol is the one xv6H implements (kernel/hypsbi.c, kernel/vmachrev.c), call for
 * call, so the guest side -- the Linux module and its patches -- does not change.
 */
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/kvm-ipc.h"
#include "kvm/irq.h"
#include "kvm/fdt.h"
#include "kvm/util.h"
#include "kvm/revise.h"
#include "kvm/migrate.h"
#include "kvm/fdt-arch.h"

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MACH_VERSION	0
#define MACH_PENDING	1
#define MACH_DESCRIBE	2
#define MACH_ACK	3
#define MACH_DTB_MAX	4
#define MACH_REQUEST	5
#define MACH_WHAT	6
#define MACH_RATE_NOW	7	/* F6: serve the described rate from now */

#define MR_STATUS	0x00	/* bit 0: a revision is pending (asked, not adopted) */
#define MR_ACK		0x04	/* write anything: lower the line */
#define MR_GEN		0x08
#define MR_WHAT		0x0c
#define MR_DTB_LO	0x10
#define MR_DTB_HI	0x14
#define MR_DTB_LEN	0x18
#define MR_VERSION	0x1c

#define SBI_SUCCESS		0
#define SBI_ERR_FAILED		-1
#define SBI_ERR_NOT_SUPPORTED	-2
#define SBI_ERR_INVALID_PARAM	-3
#define SBI_ERR_ALREADY_AVAILABLE	-6

struct kvm *revise_kvm;
struct revise_state revise;

static u64 now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* the line follows "announced and not yet taken by the driver"; caller holds the lock */
static void anchor_line(struct kvm *kvm, int level)
{
	if (revise.line == level)
		return;
	revise.line = level;
	kvm__irq_line(kvm, revise.irq, level);
}

static void anchor_mmio(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len, u8 is_write, void *ptr)
{
	struct kvm *kvm = vcpu->kvm;
	u64 off = addr - REVISE_ANCHOR_BASE;
	u32 v = 0;

	pthread_mutex_lock(&revise.lock);
	if (is_write) {
		if (off == MR_ACK)
			anchor_line(kvm, 0);
	} else {
		switch (off) {
		case MR_STATUS:	 v = revise.acked != revise.gen; break;
		case MR_GEN:	 v = revise.gen; break;
		case MR_WHAT:	 v = revise.what; break;
		case MR_DTB_LO:	 v = (u32)revise.desc_gpa; break;
		case MR_DTB_HI:	 v = revise.desc_gpa >> 32; break;
		case MR_DTB_LEN: v = REVISE_DTB_MAX; break;
		case MR_VERSION: v = 1; break;
		}
		memset(data, 0, len);
		memcpy(data, &v, min_t(u32, len, sizeof(v)));
	}
	pthread_mutex_unlock(&revise.lock);
}

/* the anchor's node and the description slots' reservation, for the device tree */
void revise__generate_fdt_nodes(void *fdt, struct kvm *kvm, void (*irq_prop)(void *, u8, enum irq_type))
{
	char name[64];
	u64 reg[2];

	if (!revise.enabled)
		return;
	snprintf(name, sizeof(name), "machrev@%llx", (unsigned long long)REVISE_ANCHOR_BASE);
	_FDT(fdt_begin_node(fdt, name));
	_FDT(fdt_property_string(fdt, "compatible", "xv6h,machrev"));
	reg[0] = cpu_to_fdt64(REVISE_ANCHOR_BASE);
	reg[1] = cpu_to_fdt64(REVISE_ANCHOR_SIZE);
	_FDT(fdt_property(fdt, "reg", reg, sizeof(reg)));
	irq_prop(fdt, revise.irq, IRQ_TYPE_LEVEL_HIGH);
	_FDT(fdt_end_node(fdt));
}

void revise__generate_reserved_memory(void *fdt, struct kvm *kvm)
{
	char name[64];
	u64 reg[2];

	if (!revise.enabled)
		return;
	_FDT(fdt_begin_node(fdt, "reserved-memory"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));
	_FDT(fdt_property(fdt, "ranges", NULL, 0));
	snprintf(name, sizeof(name), "xv6h-description@%llx", (unsigned long long)revise.base_gpa);
	_FDT(fdt_begin_node(fdt, name));
	reg[0] = cpu_to_fdt64(revise.base_gpa);
	reg[1] = cpu_to_fdt64(3 * REVISE_DTB_MAX);
	_FDT(fdt_property(fdt, "reg", reg, sizeof(reg)));
	_FDT(fdt_property(fdt, "no-map", NULL, 0));
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_end_node(fdt));
}

void revise__record_cpu(int cpu, const char *isa, unsigned long cbom, unsigned long cboz,
			unsigned long cbop, unsigned long satp, unsigned long xlen)
{
	struct revise_cpu_desc *d = &revise_cpu[cpu];

	snprintf(d->isa, sizeof(d->isa), "%s", isa);
	d->cbom = cbom; d->cboz = cboz; d->cbop = cbop; d->satp = satp; d->xlen = xlen;
	d->hartid = cpu;
	d->online = 1;
}

/* the same node generate_cpu_nodes() writes at boot, from the record */
void revise__generate_cpu_node(void *fdt, struct kvm *kvm, int cpu)
{
	struct revise_cpu_desc *d = &revise_cpu[cpu];
	static const char *mmu64[] = { [8] = "riscv,sv39", [9] = "riscv,sv48", [10] = "riscv,sv57" };
	char name[32];

	snprintf(name, sizeof(name), "cpu@%x", d->hartid);
	_FDT(fdt_begin_node(fdt, name));
	_FDT(fdt_property_string(fdt, "device_type", "cpu"));
	_FDT(fdt_property_string(fdt, "compatible", "riscv"));
	if (d->xlen == 64)
		_FDT(fdt_property_string(fdt, "mmu-type", (d->satp >= 8 && d->satp <= 10) ? mmu64[d->satp] : "riscv,none"));
	else
		_FDT(fdt_property_string(fdt, "mmu-type", d->satp == 1 ? "riscv,sv32" : "riscv,none"));
	_FDT(fdt_property_string(fdt, "riscv,isa", d->isa));
	if (d->cbom) _FDT(fdt_property_cell(fdt, "riscv,cbom-block-size", d->cbom));
	if (d->cboz) _FDT(fdt_property_cell(fdt, "riscv,cboz-block-size", d->cboz));
	if (d->cbop) _FDT(fdt_property_cell(fdt, "riscv,cbop-block-size", d->cbop));
	_FDT(fdt_property_cell(fdt, "reg", d->hartid));
	_FDT(fdt_property_string(fdt, "status", d->online ? "okay" : "disabled"));
	_FDT(fdt_begin_node(fdt, "interrupt-controller"));
	_FDT(fdt_property_string(fdt, "compatible", "riscv,cpu-intc"));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", 1));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_CPU_INTC_BASE + cpu));
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_end_node(fdt));
}

/* SBI MACH, forwarded by KVM (KVM_RISCV_SBI_EXT_MACH). Returns true when handled. */
bool revise__sbi(struct kvm_cpu *vcpu)
{
	struct kvm_run *run = vcpu->kvm_run;
	unsigned long fid = run->riscv_sbi.function_id;
	unsigned long a0 = run->riscv_sbi.args[0], a1 = run->riscv_sbi.args[1];
	long err = SBI_SUCCESS, val = 0;

	pthread_mutex_lock(&revise.lock);
	switch (fid) {
	case MACH_VERSION:
		val = 1;
		revise.capable = 1;	/* it asked, so it speaks: the legacy tier is off */
		break;
	case MACH_PENDING:
		if (!revise.t_pending) revise.t_pending = now_ns();
		val = revise.gen;
		break;
	case MACH_DESCRIBE:
		if (!revise.t_describe) revise.t_describe = now_ns();
		val = revise.desc_gpa;
		if (revise.described != revise.gen && revise.acked != revise.gen && revise.describe_hook)
			revise.describe_hook(vcpu->kvm);	/* the quiet window opens */
		revise.described = revise.gen;
		break;
	case MACH_ACK:
		/* defect 20: the ack names its generation; a stale one is refused */
		if (a1 && (u32)a1 != revise.gen) {
			revise.stale++;
			pr_info("revise: stale ack of revision %lu (the machine is on %u): refused",
				a1, revise.gen);
			err = SBI_ERR_INVALID_PARAM;
			break;
		}
		revise.acked = revise.gen;
		revise.adopted = (u32)a0;
		revise.t_done = now_ns();
		if (revise.ack_hook)
			revise.ack_hook(vcpu->kvm, revise.what, revise.adopted);	/* the facts end their aliases */
		printf("revise: revision %u adopted (0x%lx) after %llu us [pending %lld described %lld]\n",
		       revise.acked, a0, (unsigned long long)((revise.t_done - revise.t_ask) / 1000),
		       revise.t_pending ? (long long)((revise.t_pending - revise.t_ask) / 1000) : -1LL,
		       revise.t_describe ? (long long)((revise.t_describe - revise.t_ask) / 1000) : -1LL);
		fflush(stdout);
		pthread_cond_broadcast(&revise.acked_cv);
		break;
	case MACH_DTB_MAX:
		val = REVISE_DTB_MAX;
		break;
	case MACH_WHAT:
		val = revise.what;
		break;
	case MACH_RATE_NOW:
		/* F6: the guest re-bases its timekeeper now (inside its stop_machine) */
		val = revise_facts__rate_now(vcpu);
		break;
	case MACH_REQUEST:
		err = revise.request_hook ? revise.request_hook(vcpu->kvm, a0, &val) : SBI_ERR_NOT_SUPPORTED;
		break;
	default:
		err = SBI_ERR_NOT_SUPPORTED;
	}
	pthread_mutex_unlock(&revise.lock);
	run->riscv_sbi.ret[0] = err;
	run->riscv_sbi.ret[1] = val;
	return true;
}

/*
 * Ask for a revision: the facts have been changed by the caller (under the lock, through
 * their own fact code, which also sets up their aliases); write the description into the
 * free slot, bump the generation, announce. One revision at a time: wait up to 5 s for the
 * previous one's ack (the xv6H host's rule).
 */
int revise__ask(struct kvm *kvm, u32 what)
{
	struct timespec dl;
	int slot;
	u32 owed;

	pthread_mutex_lock(&revise.lock);
	if (revise.capable && revise.acked != revise.gen) {
		clock_gettime(CLOCK_REALTIME, &dl);
		dl.tv_sec += 5;
		while (revise.acked != revise.gen)
			if (pthread_cond_timedwait(&revise.acked_cv, &revise.lock, &dl))
				break;
		if (revise.acked != revise.gen) {
			pthread_mutex_unlock(&revise.lock);
			pr_warning("revise: revision %u not acknowledged in 5 s: not asking another", revise.gen);
			return -1;
		}
	}
	/* a revision asked while another is still owed carries both: the guest sees only the
	 * newest generation, and its tree has both changes in it (the KVM port's first arrival rule:
	 * "intc plic" then "timebase", and only the timebase adopted) */
	owed = revise.acked != revise.gen ? revise.what : 0;
	what |= owed;
	slot = revise.next_slot;
	revise.next_slot ^= 1;
	fprintf(stderr, "revise: building the description into slot %d at 0x%llx\n", slot, (unsigned long long)revise.slot_gpa[slot]);
	if (riscv__fdt_build(kvm, revise.slot_gpa[slot]) < 0) {
		pthread_mutex_unlock(&revise.lock);
		return -1;
	}
	revise.desc_gpa = revise.slot_gpa[slot];
	revise.what = what;
	revise.adopted = 0;
	revise.t_pending = revise.t_describe = revise.t_done = 0;
	revise.t_ask = now_ns();
	revise.gen++;
	fprintf(stderr, "revise: built; raising the anchor\n");
	anchor_line(kvm, 1);
	printf("revise: revision %u asked for (what=0x%x)\n", revise.gen, what);
	fflush(stdout);
	pthread_mutex_unlock(&revise.lock);
	return 0;
}

/*
 * `lkvm revise --name VM <fact> <value>`: the request comes over the IPC socket as one
 * NUL-terminated string "fact value"; the fact table (riscv/revise-facts.c) applies it.
 */
static void revise_ipc(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	char buf[256], reply[256];
	int r;

	fprintf(stderr, "revise: ipc request (%u bytes)\n", len);
	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, msg, len);
	buf[len] = 0;
	r = revise__apply(kvm, buf, reply, sizeof(reply));
	if (write(fd, reply, strlen(reply) + 1) < 0)
		pr_warning("revise: reply not sent");
	(void)r;
}

/* the slots, the anchor and the IPC handler; runs after the RAM, before the FDT */
static int revise__init(struct kvm *kvm)
{
	if (!kvm->cfg.arch.revise)
		return 0;
	revise_kvm = kvm;
	revise.enabled = 1;
	pthread_mutex_init(&revise.lock, NULL);
	pthread_cond_init(&revise.acked_cv, NULL);
	revise.irq = irq__alloc_line();
	revise.base_gpa = kvm->arch.dtb_guest_start;		/* the boot blob, never rewritten */
	revise.slot_gpa[0] = revise.base_gpa + REVISE_DTB_MAX;	/* slot A */
	revise.slot_gpa[1] = revise.base_gpa + 2 * REVISE_DTB_MAX;	/* slot B */
	revise.desc_gpa = revise.base_gpa;
	/* both slots whole inside guest RAM: guest_flat_to_host checks only an address */
	for (int s = 0; s < 2; s++)
		if (!host_ptr_in_ram(kvm, guest_flat_to_host(kvm, revise.slot_gpa[s])) ||
		    !host_ptr_in_ram(kvm, guest_flat_to_host(kvm, revise.slot_gpa[s] + REVISE_DTB_MAX - 1)))
			die("revise: description slot %d at 0x%llx is not in guest RAM", s,
			    (unsigned long long)revise.slot_gpa[s]);
	if (kvm__register_mmio(kvm, REVISE_ANCHOR_BASE, REVISE_ANCHOR_SIZE, false, anchor_mmio, NULL) < 0)
		die("revise: cannot register the anchor at 0x%llx", (unsigned long long)REVISE_ANCHOR_BASE);
	kvm_ipc__register_handler(KVM_IPC_REVISE, revise_ipc);
	pr_info("revise: on; anchor at 0x%llx line %d, description slots at 0x%llx",
		(unsigned long long)REVISE_ANCHOR_BASE, revise.irq, (unsigned long long)revise.base_gpa);
	return revise_facts__init(kvm);
}
dev_base_init(revise__init);

/* A.6: the revision state across a migration: the generation and what was acknowledged, the
 * slots (their contents are guest RAM, copied with it), what the description says of each cpu,
 * and the facts' own state. A revision in flight is not migrated: one is asked only when none
 * is owed, and lkvm migrate is the asker's to time. */
struct revise_mig {
	u32 gen, acked, described, what, adopted, stale;
	u64 desc_gpa;
	int next_slot, nrcpus, capable;
	u8 facts[256];
	int nfacts;
	struct revise_cpu_desc cpu[];
};

int revise__state_send(int fd, mig_put_t put)
{
	struct kvm *kvm = revise_kvm;
	size_t n;
	struct revise_mig *m;
	int r;

	if (!revise.enabled || !kvm)
		return 0;
	n = sizeof(*m) + kvm->nrcpus * sizeof(struct revise_cpu_desc);
	m = calloc(1, n);
	if (!m)
		return -1;
	pthread_mutex_lock(&revise.lock);
	m->gen = revise.gen; m->acked = revise.acked; m->described = revise.described;
	m->what = revise.what; m->adopted = revise.adopted; m->stale = revise.stale;
	m->desc_gpa = revise.desc_gpa; m->next_slot = revise.next_slot; m->nrcpus = kvm->nrcpus;
	m->capable = revise.capable;	/* the guest announced itself there: it is the same guest here */
	memcpy(m->cpu, revise_cpu, kvm->nrcpus * sizeof(struct revise_cpu_desc));
	m->nfacts = revise_facts__state_get(m->facts, sizeof(m->facts));
	pthread_mutex_unlock(&revise.lock);
	r = put(fd, T_REVISE, m, n);
	free(m);
	return r;
}

void revise__state_apply(struct kvm *kvm, void *buf, u64 len)
{
	struct revise_mig *m = buf;

	/* the record from the other host, checked before any of it is used: its cpus are this
	 * machine's, the slot to write next is one of the two, and the description the guest is on
	 * is the boot blob or a slot. An unchecked next_slot would have sent the next revision's
	 * tree to any address the stream named. */
	if (!revise.enabled || len < sizeof(*m) || m->nrcpus != kvm->nrcpus ||
	    len < sizeof(*m) + (u64)kvm->nrcpus * sizeof(struct revise_cpu_desc) ||
	    (m->next_slot != 0 && m->next_slot != 1) ||
	    (m->desc_gpa != revise.base_gpa && m->desc_gpa != revise.slot_gpa[0] && m->desc_gpa != revise.slot_gpa[1]) ||
	    m->nfacts < 0 || m->nfacts > (int)sizeof(m->facts))
		die("revise: the revision state that came with the machine does not fit here");
	pthread_mutex_lock(&revise.lock);
	revise.gen = m->gen; revise.acked = m->acked; revise.described = m->described;
	revise.what = m->what; revise.adopted = m->adopted; revise.stale = m->stale;
	revise.desc_gpa = m->desc_gpa; revise.next_slot = m->next_slot;
	revise.capable = m->capable;
	memcpy(revise_cpu, m->cpu, kvm->nrcpus * sizeof(struct revise_cpu_desc));
	for (int i = 0; i < kvm->nrcpus; i++)
		revise_cpu[i].isa[sizeof(revise_cpu[i].isa) - 1] = 0;
	revise_cpu_cached = 1;
	pthread_mutex_unlock(&revise.lock);
	revise_facts__state_put(kvm, m->facts, m->nfacts);
}
