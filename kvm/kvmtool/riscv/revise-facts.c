/*
 * The facts kvmtool can revise, one function each. A fact function runs under revise.lock,
 * changes the machine (and sets up its alias: the old value keeps answering until the ack),
 * and returns the REV_* bits the guest must look at; the ack hook ends the aliases.
 * Facts: device map (F15), interrupt numbering (F3), controller (F1), timebase (F6),
 * vector (F10).
 */
#include "kvm/kvm.h"
#include "kvm/util.h"
#include "kvm/revise.h"
#include "kvm/irq.h"
#include "kvm/virtio-mmio.h"
#include "kvm/fdt.h"
#include "kvm/kvm-arch.h"
#include "kvm/kvm-cpu.h"
#include "kvm/migrate.h"

#include <linux/kvm.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

struct fact {
	const char *name;
	int (*apply)(struct kvm *kvm, const char *value, char *reply, int len);	/* <0: refused */
	bool unlocked;	/* runs without revise.lock: it waits on the vcpus, which may be waiting on it */
};

/* a revision that changes nothing: the protocol's round trip, alone */
static int fact_nochange(struct kvm *kvm, const char *value, char *reply, int len)
{
	return 0;
}

/*
 * F15: every virtio-mmio window a megabyte up ("alt") or back ("default"), as xv6H's
 * `vmm revise VM map alt|default`. The old windows and their notify eventfds keep answering
 * until the ack.
 */
#define DEVMAP_SHIFT 0x100000
static int devmap_alt, devmap_was;
static int fact_devmap(struct kvm *kvm, const char *value, char *reply, int len)
{
	int want = !strcmp(value, "alt");

	if (!want && strcmp(value, "default")) {
		snprintf(reply, len, "revise: devmap alt|default");
		return -1;
	}
	if (want == devmap_alt)
		return 0;
	if (virtio_mmio_relocate_all(kvm, want ? DEVMAP_SHIFT : -DEVMAP_SHIFT) < 0) {
		snprintf(reply, len, "revise: a device move is still owed");
		return -1;
	}
	devmap_was = devmap_alt;
	devmap_alt = want;
	return REV_DEVICES;
}

/*
 * F3: the virtio devices' lines renumbered onto spare lines ("alt") or back ("default"), as
 * xv6H's `vmm revise VM irqmap alt|default`. A device raises the line the guest knows until
 * the ack; between the guest's describe() and its ack, edges are held and raised at the ack
 * on the new line (the quiet window).
 */
#define SPARE_LINES 8
static u8 spare[SPARE_LINES], orig[SPARE_LINES];
static int norig, irqmap_alt, irqmap_was;
static int fact_irqmap(struct kvm *kvm, const char *value, char *reply, int len)
{
	int want = !strcmp(value, "alt");

	if (!want && strcmp(value, "default")) {
		snprintf(reply, len, "revise: irqmap alt|default");
		return -1;
	}
	if (want == irqmap_alt)
		return 0;
	if (virtio_mmio_count() > SPARE_LINES) {
		snprintf(reply, len, "revise: more devices than spare lines");
		return -1;
	}
	if (!norig)
		norig = virtio_mmio_lines(orig, SPARE_LINES);	/* the boot numbering */
	if (virtio_mmio_renumber_all(kvm, want ? spare : orig, SPARE_LINES) < 0)
		return -1;
	irqmap_was = irqmap_alt;
	irqmap_alt = want;
	return REV_DEVICES;
}

/*
 * F1: the controller's kind, between KVM's in-kernel AIA (an APLIC in MSI mode with an IMSIC
 * per hart: "imsic") and kvmtool's userspace PLIC ("plic"). Both are resident from boot, so
 * nothing is created or torn down under a running VM; the description names one through an
 * xv6h,vic node (the Linux guest's patch 0005 binds it and re-binds on REV_INTC). Lines are
 * raised on the controller the guest still uses until it acknowledges; at the ack every line
 * goes to the new one, and every line still owed is raised there again from the device
 * models (Theorem 1): each level a device holds (riscv_irq_level) and each virtio-mmio
 * device whose InterruptStatus is not acknowledged. The old controller's lines are lowered.
 */
enum { INTC_IMSIC = 0, INTC_PLIC = 1 };
static const char *const intc_name[] = { "imsic", "plic" };
static int intc_desc = INTC_IMSIC, intc_live = INTC_IMSIC;
static bool intc_on;
#define REVISE_PLIC_BASE 0x0c000000ULL
#define REVISE_PLIC_SIZE 0x04000000ULL

static void line_on(struct kvm *kvm, int kind, int irq, int level, bool edge)
{
	struct kvm_irq_level l = { .irq = irq };

	if (kind == INTC_PLIC) {
		plic__irq_trig(kvm, irq, level, edge);
		return;
	}
	l.level = edge ? 1 : !!level;
	if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &l) < 0)
		pr_warning("revise: KVM_IRQ_LINE %d failed", irq);
	if (edge) {
		l.level = 0;
		ioctl(kvm->vm_fd, KVM_IRQ_LINE, &l);
	}
}

static void intc_route(struct kvm *kvm, int irq, int level, bool edge)
{
	line_on(kvm, intc_live, irq, level, edge);
}

static void intc_fdt(void *fdt, struct kvm *kvm)
{
	u64 base = intc_desc == INTC_PLIC ? REVISE_PLIC_BASE
					  : RISCV_IRQCHIP + (u64)kvm->nrcpus * KVM_DEV_RISCV_IMSIC_SIZE;
	u64 size = intc_desc == INTC_PLIC ? REVISE_PLIC_SIZE : KVM_DEV_RISCV_APLIC_SIZE;
	u32 reg[4] = { cpu_to_fdt32(base >> 32), cpu_to_fdt32((u32)base),
		       cpu_to_fdt32(size >> 32), cpu_to_fdt32((u32)size) };
	u32 *cells = calloc(kvm->nrcpus * 2, sizeof(u32));
	char name[64];

	if (!cells)
		die("revise: no memory for the controller node");
	snprintf(name, sizeof(name), "interrupt-controller@%08llx", (unsigned long long)base);
	_FDT(fdt_begin_node(fdt, name));
	_FDT(fdt_property_string(fdt, "compatible", "xv6h,vic"));
	_FDT(fdt_property_string(fdt, "xv6h,kind", intc_name[intc_desc]));
	_FDT(fdt_property(fdt, "reg", reg, sizeof(reg)));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", 1));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0));
	for (int i = 0; i < kvm->nrcpus; i++) {
		cells[2 * i] = cpu_to_fdt32(PHANDLE_CPU_INTC_BASE + i);
		cells[2 * i + 1] = cpu_to_fdt32(9);	/* each hart's supervisor external interrupt */
	}
	_FDT(fdt_property(fdt, "interrupts-extended", cells, sizeof(u32) * kvm->nrcpus * 2));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_PLIC));
	_FDT(fdt_end_node(fdt));
	free(cells);
}

static int fact_intc(struct kvm *kvm, const char *value, char *reply, int len)
{
	int want = !strcmp(value, "plic") ? INTC_PLIC :
		   (!strcmp(value, "imsic") || !strcmp(value, "aia")) ? INTC_IMSIC : -1;

	if (want < 0) {
		snprintf(reply, len, "revise: intc plic|imsic");
		return -1;
	}
	if (!intc_on) {
		snprintf(reply, len, "revise: this machine has one controller (needs the in-kernel AIA)");
		return -1;
	}
	if (want == intc_desc)
		return 0;
	intc_desc = want;
	return REV_INTC;
}

/* the guest took the new controller: the lines follow, and what is owed is raised again */
static void intc_switch(struct kvm *kvm)
{
	/* lines are 1 .. irq__get_nr_allocated_lines(): the count is the last line's number (one
	 * short, the last line allocated -- the disk -- would keep its level on the old PLIC: an
	 * interrupt storm when the guest comes back to it) */
	int old = intc_live, nlines = irq__get_nr_allocated_lines(), lv = 0, vio;

	if (intc_live == intc_desc)
		return;
	for (int irq = 1; irq <= nlines && irq < 256; irq++)
		line_on(kvm, old, irq, 0, false);	/* the old model holds nothing */
	intc_live = intc_desc;
	if (intc_live == INTC_PLIC)
		plic__reset_state(kvm);			/* no claim left over from its last epoch */
	for (int irq = 1; irq <= nlines && irq < 256; irq++)
		if (riscv_irq_level[irq]) {
			line_on(kvm, intc_live, irq, 1, false);
			lv++;
		}
	vio = virtio_mmio_reraise_all(kvm);
	fprintf(stderr, "revise: intc %s -> %s: %d held level(s) and %d virtio status(es) raised again\n",
		intc_name[old], intc_name[intc_live], lv, vio);
}

/*
 * F6: the timebase. KVM (patch 2) runs the guest's counter at the new rate from where it is;
 * the rate is set on every vcpu from its own thread (a ONE_REG on a vcpu waits for it to leave
 * KVM_RUN, and an idle vcpu never does by itself). The description says the new
 * timebase-frequency, and no Sstc while the rate is not the host's (the comparator counts at
 * the host's rate): the guest moves its timer to SBI (REV_TIMER) and back.
 */
static u64 native_hz;
static void set_rate(struct kvm_cpu *vcpu, void *data)
{
	u64 hz = *(u64 *)data;
	struct kvm_one_reg reg = { .id = RISCV_TIMER_REG(frequency), .addr = (u64)(unsigned long)&hz };

	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		pr_warning("revise: vcpu %lu: timer frequency %llu refused: %s", vcpu->cpu_id,
			   (unsigned long long)hz, strerror(errno));
}

static void isa_sstc(char *isa, size_t len, bool on)
{
	char *p = strstr(isa, "_sstc");

	if (!on && p)
		memmove(p, p + 5, strlen(p + 5) + 1);
	else if (on && !p && strlen(isa) + 6 < len)
		strcat(isa, "_sstc");
}

/*
 * The rate is described at the ask and served from the guest's read of the description
 * (facts_describe), as in xv6H: changing KVM's rate at the ask would run the guest's clock at the
 * new rate while its code still converts with the old, so every timer it arms is already past,
 * and a guest slow to read the announcement could not get to it.
 */
static u64 tb_owed;
static struct kvm *tb_kvm;

static void *tb_switch_thread(void *arg)
{
	static struct kvm_cpu_task task = { .func = set_rate };
	static u64 hz;

	hz = *(u64 *)arg;
	task.data = &hz;
	kvm_cpu__run_on_all_cpus(tb_kvm, &task);
	fprintf(stderr, "revise: timebase served at %llu Hz from the guest's read\n", (unsigned long long)hz);
	return NULL;
}

static int fact_timebase(struct kvm *kvm, const char *value, char *reply, int len)
{
	char *end;
	u64 want = strtoull(value, &end, 0);

	if (!value[0] || *end || want < 1000 || want > 1000000000ULL) {
		snprintf(reply, len, "revise: timebase HZ (1000..1000000000, or the host's %llu)",
			 (unsigned long long)native_hz);
		return -1;
	}
	if (want == kvm->cpus[0]->riscv_timebase)
		return 0;
	tb_kvm = kvm;
	pthread_mutex_lock(&revise.lock);
	tb_owed = want;
	for (int i = 0; i < kvm->nrcpus; i++) {
		kvm->cpus[i]->riscv_timebase = want;
		isa_sstc(revise_cpu[i].isa, sizeof(revise_cpu[i].isa), want == native_hz);
	}
	pthread_mutex_unlock(&revise.lock);
	fprintf(stderr, "revise: timebase -> %llu Hz (%s)\n", (unsigned long long)want,
		want == native_hz ? "the host's: Sstc again" : "scaled: time by trap, the timer by SBI");
	return REV_TIMER;
}

/*
 * F10: the vector unit (KVM patch 3). Off: the description loses V (the cpu nodes' "v" and their
 * zv* extensions), the guest cedes the unit and acknowledges, and only then is V turned off in
 * KVM (so a guest that did not cede keeps a working unit). On: V is turned on in KVM first, then
 * described. F11 (another width) is refused: the hardware's VLEN is fixed.
 */
static char isa_with_v[KVM_NR_CPUS][1024];
static int vec_off, vec_off_owed;

static void set_v(struct kvm_cpu *vcpu, void *data)
{
	u64 on = *(u64 *)data;
	struct kvm_one_reg reg = { .id = RISCV_ISA_EXT_REG(KVM_RISCV_ISA_EXT_V), .addr = (u64)(unsigned long)&on };

	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		pr_warning("revise: vcpu %lu: V %s refused: %s", vcpu->cpu_id, on ? "on" : "off", strerror(errno));
}

static void kvm_v(struct kvm *kvm, int on)
{
	static struct kvm_cpu_task task = { .func = set_v };
	static u64 val;

	val = on;
	task.data = &val;
	kvm_cpu__run_on_all_cpus(kvm, &task);
}

static void *v_off_thread(void *arg)
{
	kvm_v(arg, 0);
	fprintf(stderr, "revise: the guest ceded the vector unit: V off in KVM\n");
	return NULL;
}

/* "rv64imafdcvh_zicsr_..._zve64d_zvl128b..." -> without the unit */
static void isa_drop_v(char *isa)
{
	char out[1024], *p = isa, *o = out;
	int base = 1;

	for (; *p; p++) {
		if (*p == '_') {
			base = 0;
			if (!strncmp(p, "_zv", 3)) {		/* a vector extension: skipped whole */
				while (p[1] && p[1] != '_')
					p++;
				continue;
			}
		}
		if (base && p - isa >= 4 && *p == 'v')
			continue;
		*o++ = *p;
	}
	*o = 0;
	strcpy(isa, out);
}

static int fact_vector(struct kvm *kvm, const char *value, char *reply, int len)
{
	int off = !strcmp(value, "off");

	if (!off && strcmp(value, "on")) {
		snprintf(reply, len, "revise: vector on|off (another width is not this hardware's to give)");
		return -1;
	}
	if (off == vec_off)
		return 0;
	if (!isa_with_v[0][0])
		for (int i = 0; i < kvm->nrcpus; i++)
			memcpy(isa_with_v[i], revise_cpu[i].isa, sizeof(isa_with_v[i]));
	if (!off)
		kvm_v(kvm, 1);			/* the unit first, then the description */
	pthread_mutex_lock(&revise.lock);
	for (int i = 0; i < kvm->nrcpus; i++) {
		memcpy(revise_cpu[i].isa, isa_with_v[i], sizeof(revise_cpu[i].isa));
		if (off)
			isa_drop_v(revise_cpu[i].isa);
	}
	vec_off = off;
	vec_off_owed = off;
	pthread_mutex_unlock(&revise.lock);
	return REV_VLEN;
}

static const struct fact facts[] = {
	{ "nochange", fact_nochange },
	{ "devmap", fact_devmap },
	{ "irqmap", fact_irqmap },
	{ "intc", fact_intc },
	{ "timebase", fact_timebase, true },
	{ "vector", fact_vector, true },
};

/* the guest has read the description: a renumbering being taken holds the lines (F3) */
static void facts_describe(struct kvm *kvm)
{
	virtio_mmio_quiet_all(kvm, true);
}

/*
 * F6: serve the described rate from now. The guest asks (MACH call 7) inside the stop_machine in
 * which it re-bases its timekeeper; a guest that does not ask gets it at its ack. The rate is
 * per VM in KVM (patch 2), so setting it on the asking vcpu, from its own thread, switches the
 * counter at this instant; the other vcpus take it from a thread (run_on_all_cpus waits for the
 * asking vcpu too, which is here).
 */
int revise_facts__rate_now(struct kvm_cpu *vcpu)
{
	static u64 hz;
	pthread_t th;

	if (!tb_owed)
		return 0;
	hz = tb_owed;
	tb_owed = 0;
	if (vcpu)
		set_rate(vcpu, &hz);
	pthread_create(&th, NULL, tb_switch_thread, &hz);
	pthread_detach(th);
	return 1;
}

/* the guest acknowledged: every alias ends (F15 windows, F3 lines) */
static void facts_ack(struct kvm *kvm, u32 what, u32 adopted)
{
	revise_facts__rate_now(NULL);	/* F6: a guest that did not ask for the rate at its re-base */
	if ((what & REV_DEVICES) && !(adopted & REV_DEVICES)) {
		/* not taken: the windows and lines stay where the guest has them, and so does the description */
		virtio_mmio_relocate_cancel(kvm);
		virtio_mmio_renumber_cancel(kvm);
		devmap_alt = devmap_was;
		irqmap_alt = irqmap_was;
		fprintf(stderr, "revise: the device revision was not taken: windows and lines kept\n");
	} else {
		virtio_mmio_relocate_end(kvm);
		virtio_mmio_renumber_end(kvm);
	}
	if (adopted & REV_INTC)
		intc_switch(kvm);
	if (vec_off_owed && (what & REV_VLEN)) {
		vec_off_owed = 0;
		if (adopted & REV_VLEN) {
			pthread_t th;	/* not from here: this is a vcpu thread holding the revision lock */

			pthread_create(&th, NULL, v_off_thread, kvm);
			pthread_detach(th);
		} else {
			fprintf(stderr, "revise: the guest kept the vector unit: V stays on\n");
		}
	}
	else if (intc_desc != intc_live)
		intc_desc = intc_live;		/* not taken: the description goes back to what is live */
}



/*
 * The in-kernel AIA reconfigured under a running guest. Every vCPU's
 * IMSIC moves from its hardware guest file to KVM's software file ("sw") or back ("hw"), by KVM
 * patch 4 (KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW). Nothing is asked of the guest: like F23 under
 * xv6H, the description does not change, only what backs it, so no revision is made.
 */
#ifndef KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW
#define KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW 1
#endif
static int imsicfile(const char *value, char *reply, int len)
{
	u32 sw = !strcmp(value, "sw");
	struct kvm_device_attr a = {
		.group = KVM_DEV_RISCV_AIA_GRP_CTRL,
		.attr = KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW,
		.addr = (u64)(unsigned long)&sw,
	};

	if (!sw && strcmp(value, "hw")) {
		snprintf(reply, len, "revise: imsicfile hw|sw");
		return -1;
	}
	if (aia__fd() < 0 || ioctl(aia__fd(), KVM_SET_DEVICE_ATTR, &a) < 0) {
		snprintf(reply, len, "revise: imsicfile %s: refused by KVM: %s", value,
			 aia__fd() < 0 ? "no in-kernel AIA" : strerror(errno));
		return -1;
	}
	snprintf(reply, len, "revise: imsicfile %s: every vCPU's IMSIC now on %s files; nothing asked of the guest",
		 value, sw ? "software" : "hardware");
	return 0;
}

int revise__apply(struct kvm *kvm, const char *req, char *reply, int len)
{
	char name[64] = "", value[128] = "";
	int what = -1;

	if (sscanf(req, "%63s %127s", name, value) < 1) {
		snprintf(reply, len, "revise: usage: <fact> [value]");
		return -1;
	}
	if (!strcmp(name, "imsicfile"))
		return imsicfile(value, reply, len);
	for (unsigned i = 0; i < sizeof(facts) / sizeof(facts[0]); i++)
		if (!strcmp(facts[i].name, name)) {
			if (facts[i].unlocked) {
				what = facts[i].apply(kvm, value, reply, len);
			} else {
				pthread_mutex_lock(&revise.lock);
				what = facts[i].apply(kvm, value, reply, len);
				pthread_mutex_unlock(&revise.lock);
			}
			break;
		}
	if (what < 0) {
		if (!reply[0])
			snprintf(reply, len, "revise: no fact %s (or refused)", name);
		return -1;
	}
	if (revise__ask(kvm, what) < 0) {
		snprintf(reply, len, "revise: not asked (a revision is still owed)");
		return -1;
	}
	snprintf(reply, len, "revise: %s %s: revision %u asked for (what=0x%x)", name, value, revise.gen, what);
	return 0;
}

int revise_facts__init(struct kvm *kvm)
{
	revise.ack_hook = facts_ack;
	revise.describe_hook = facts_describe;
	/* F3's spare lines: allocated before the devices, so the irqchip has them from boot */
	for (int i = 0; i < SPARE_LINES; i++)
		spare[i] = irq__alloc_line();
	/* F1: with the in-kernel AIA, kvmtool's PLIC is resident too (plic.c), and the tree names
	 * one of them; XV6H_INTC=plic boots on the PLIC */
	native_hz = kvm->cpus[0] ? kvm->cpus[0]->riscv_timebase : 0;
	if (riscv_irqchip == IRQCHIP_AIA) {
		const char *k = getenv("XV6H_INTC");

		intc_on = true;
		intc_desc = intc_live = (k && !strcmp(k, "plic")) ? INTC_PLIC : INTC_IMSIC;
		riscv_irqchip_route = intc_route;
		riscv_irqchip_generate_fdt_node = intc_fdt;
		riscv_irqchip_phandle = PHANDLE_PLIC;
		riscv_irqchip_line_sensing = false;	/* one cell: the vic's */
		fprintf(stderr, "revise: controller revisable (imsic | plic), booting on %s\n", intc_name[intc_desc]);
	}
	return 0;
}

/* The facts' state across a migration (what the description says now; the windows and
 * lines themselves travel with the devices) */
struct facts_mig {
	int devmap_alt, irqmap_alt, intc_desc, intc_live;
	u64 hz;
};

int revise_facts__state_get(void *buf, int max)
{
	struct facts_mig f = { devmap_alt, irqmap_alt, intc_desc, intc_live,
			       revise_kvm && revise_kvm->cpus[0] ? revise_kvm->cpus[0]->riscv_timebase : 0 };

	if (max < (int)sizeof(f))
		return 0;
	memcpy(buf, &f, sizeof(f));
	return sizeof(f);
}

void revise_facts__state_put(struct kvm *kvm, const void *buf, int len)
{
	struct facts_mig f;

	if (len != (int)sizeof(f))
		return;
	memcpy(&f, buf, sizeof(f));
	/* intc_desc and intc_live index tables; a rate is a rate (the survey's range, and then some) */
	if ((unsigned)f.intc_desc >= ARRAY_SIZE(intc_name) || (unsigned)f.intc_live >= ARRAY_SIZE(intc_name) ||
	    (f.hz && (f.hz < 1000 || f.hz > 10000000000ULL)))
		die("revise: the facts that came with the machine do not fit here");
	devmap_alt = !!f.devmap_alt;
	irqmap_alt = !!f.irqmap_alt;
	if (intc_on) {
		intc_desc = f.intc_desc;
		intc_live = f.intc_live;
	}
	if (f.hz)
		for (int i = 0; i < kvm->nrcpus; i++)
			kvm->cpus[i]->riscv_timebase = f.hz;	/* the clock's rate itself is in the timer registers */
}
