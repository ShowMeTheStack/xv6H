// SPDX-License-Identifier: GPL-2.0
/*
 * machrev: a Linux guest of xv6H takes revisions of its machine description.
 *
 * The hypervisor may change the machine under a running guest -- its RAM,
 * its hart count, the cache-block size, its timer, its devices' addresses --
 * and rewrites the device tree it gave the guest at boot, then announces the
 * revision. An xv6 guest hears it as a supervisor software interrupt; Linux
 * hears it through the one device promised never to move (the anchor,
 * compatible "xv6h,machrev"). This driver takes the announcement, reads the
 * new tree back (SBI extension "MACH", 0x4D414348), diffs it against what it
 * knows, applies what a stock kernel can apply, prints what it did, and
 * acknowledges with the mask of what it adopted. What it cannot do without a
 * kernel patch it says so, and leaves out of the mask; the host then knows.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/memory_hotplug.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/irqdomain.h>
#include <linux/ioport.h>
#include <asm/sbi.h>
#include <asm/cacheflush.h>

/* linux/patches/0001-riscv-timer-revise.patch */
extern void riscv_timer_revise(bool sstc, u32 hz);

#define SBI_EXT_MACH 0x4D414348
#define MACH_VERSION 0
#define MACH_PENDING 1
#define MACH_DESCRIBE 2
#define MACH_ACK 3
#define MACH_DTB_MAX 4
#define MACH_WHAT 6

#define REV_DEVICES (1u << 0)
#define REV_INTC    (1u << 1)
#define REV_VLEN    (1u << 2)
#define REV_SBI     (1u << 3)
#define REV_TIMER   (1u << 4)
#define REV_MEM     (1u << 5)
#define REV_MEMORY  (1u << 6)

#define MR_STATUS 0x00
#define MR_ACK    0x04

#define MAX_HARTS 64
#define MAX_VIRTIO 8

struct facts {
	int ncpu, online;
	u32 hartid[MAX_HARTS];
	bool parked[MAX_HARTS];
	u64 ram_base, ram_size;
	u32 cboz, timebase;
	bool sstc;
	int nvirtio;
	u64 virtio_base[MAX_VIRTIO];
	u32 virtio_irq[MAX_VIRTIO];
	bool rtc;
	u64 rtc_base;
	u32 rtc_irq;
	const char *intc;
};

struct machrev {
	struct device *dev;
	void __iomem *regs;
	int irq;
	struct facts cur;
	unsigned long gen;
	unsigned long revisions, adopted_total;
};

static struct sbiret mach(unsigned long fid, unsigned long a0)
{
	return sbi_ecall(SBI_EXT_MACH, fid, a0, 0, 0, 0, 0, 0);
}

/* what the tree says the machine is; the same reading for the live tree and a revised one */
static void read_facts(struct device_node *root, struct facts *f)
{
	struct device_node *n, *cpus;
	u64 reg[2];

	memset(f, 0, sizeof(*f));
	cpus = of_get_child_by_name(root, "cpus");
	if (cpus) {
		of_property_read_u32(cpus, "timebase-frequency", &f->timebase);
		for_each_child_of_node(cpus, n) {
			const char *isa = NULL;
			u32 hid = 0;

			if (!of_node_name_eq(n, "cpu") || f->ncpu >= MAX_HARTS)
				continue;
			of_property_read_u32(n, "reg", &hid);
			f->hartid[f->ncpu] = hid;
			f->parked[f->ncpu] = !of_device_is_available(n) ||
					     of_property_read_bool(n, "xv6h,parked");
			if (!f->parked[f->ncpu])
				f->online++;
			f->ncpu++;
			of_property_read_u32(n, "riscv,cboz-block-size", &f->cboz);
			if (!of_property_read_string(n, "riscv,isa", &isa) && isa)
				f->sstc = strstr(isa, "sstc") != NULL;
		}
		of_node_put(cpus);
	}
	n = of_get_child_by_name(root, "memory");
	if (n) {
		/* returns the count read, not 0, when it succeeds */
		if (of_property_read_variable_u64_array(n, "reg", reg, 2, 2) == 2) {
			f->ram_base = reg[0];
			f->ram_size = reg[1];
		}
		of_node_put(n);
	}
	/* the devices live under /soc (xv6H puts them there, as QEMU's virt does) */
	for_each_child_of_node(root, n) {
		struct device_node *soc = of_node_name_eq(n, "soc") ? n : NULL, *c;

		if (!soc)
			continue;
		for_each_child_of_node(soc, c) {
			if (of_device_is_compatible(c, "virtio,mmio") && f->nvirtio < MAX_VIRTIO) {
				if (of_property_read_variable_u64_array(c, "reg", reg, 2, 2) == 2)
					f->virtio_base[f->nvirtio] = reg[0];
				of_property_read_u32_index(c, "interrupts", 0, &f->virtio_irq[f->nvirtio]);
				f->nvirtio++;
			} else if (of_device_is_compatible(c, "google,goldfish-rtc")) {
				f->rtc = true;
				if (of_property_read_variable_u64_array(c, "reg", reg, 2, 2) == 2)
					f->rtc_base = reg[0];
				of_property_read_u32_index(c, "interrupts", 0, &f->rtc_irq);
			} else if (of_device_is_compatible(c, "riscv,plic0") || of_device_is_compatible(c, "sifive,plic-1.0.0")) {
				f->intc = "plic";
			} else if (of_device_is_compatible(c, "riscv,aplic")) {
				f->intc = f->intc && !strcmp(f->intc, "imsic") ? f->intc : "aplic";
			} else if (of_device_is_compatible(c, "riscv,imsics")) {
				f->intc = "imsic";
			}
		}
	}
}

/* the logical cpu that is hart `hart', from the live tree (cpu numbering is Linux's own) */
static int cpu_of_hart(u32 hart)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct device_node *n = of_get_cpu_node(cpu, NULL);
		u32 hid = ~0u;
		int r = -1;

		if (n)
			r = of_property_read_u32(n, "reg", &hid);
		if (n && r == 0 && hid == hart) {
			of_node_put(n);
			return cpu;
		}
		pr_info("machrev: looking for hart %u: cpu %d is %pOF (reg %u, %d)\n", hart, cpu, n, hid, r);
		of_node_put(n);
	}
	return -1;
}

/*
 * F16/F17: the RTC comes or goes. Linux's goldfish driver is a platform
 * driver, and platform devices can be made and unmade at run time: the
 * one the boot tree made is unregistered when the device goes; when it
 * comes back a fresh one is registered by hand (its interrupt mapped on
 * the live tree's controller), and the driver binds by name.
 */
static struct platform_device *machrev_rtc;

static void rtc_gone(void)
{
	struct device *dev = bus_find_device_by_name(&platform_bus_type, NULL, "101000.rtc");

	if (dev) {
		platform_device_unregister(to_platform_device(dev));
		put_device(dev);
		pr_info("machrev: rtc gone: the boot tree's goldfish_rtc unregistered\n");
	} else if (machrev_rtc) {
		platform_device_unregister(machrev_rtc);
		machrev_rtc = NULL;
		pr_info("machrev: rtc gone: goldfish_rtc unregistered\n");
	} else {
		pr_info("machrev: rtc gone (had none)\n");
	}
}

static void rtc_back(u64 base, u32 hwirq)
{
	struct device_node *ic;
	struct irq_fwspec fwspec = { };
	struct resource res[2];
	int virq;

	ic = of_find_compatible_node(NULL, NULL, "riscv,plic0");
	if (!ic)
		ic = of_find_compatible_node(NULL, NULL, "sifive,plic-1.0.0");
	if (!ic)
		ic = of_find_compatible_node(NULL, NULL, "riscv,aplic");
	if (!ic) {
		pr_info("machrev: rtc back, but no interrupt controller node to map its line on\n");
		return;
	}
	fwspec.fwnode = of_node_to_fwnode(ic);
	fwspec.param[0] = hwirq;
	fwspec.param[1] = IRQ_TYPE_LEVEL_HIGH;
	fwspec.param_count = of_device_is_compatible(ic, "riscv,aplic") ? 2 : 1;
	virq = irq_create_fwspec_mapping(&fwspec);
	of_node_put(ic);
	memset(res, 0, sizeof(res));
	res[0] = DEFINE_RES_MEM(base, 0x1000);
	res[1] = DEFINE_RES_IRQ(virq);
	machrev_rtc = platform_device_register_resndata(NULL, "goldfish_rtc", -1, res, 2, NULL, 0);
	if (IS_ERR(machrev_rtc)) {
		pr_info("machrev: rtc back at 0x%llx, registering it failed: %ld\n", base, PTR_ERR(machrev_rtc));
		machrev_rtc = NULL;
		return;
	}
	pr_info("machrev: rtc back at 0x%llx line %u (irq %d): goldfish_rtc registered\n", base, hwirq, virq);
}

/* what this kernel can take, taken; what it cannot, said */
static u32 apply(struct machrev *m, const struct facts *new, u32 what)
{
	const struct facts *old = &m->cur;
	u32 adopted = 0;
	int i;

	/* F21: the cache-block size cbo.zero clears. clear_page reads the variable on every page. */
	if (new->cboz && new->cboz != riscv_cboz_block_size) {
		pr_info("machrev: cache block %u -> %u bytes\n", riscv_cboz_block_size, new->cboz);
		riscv_cboz_block_size = new->cboz;
		adopted |= REV_DEVICES;
	}
	/* F13: more RAM. The memory node grew; the new range is hot-added and onlined. */
	if (new->ram_size > old->ram_size && new->ram_base == old->ram_base) {
		u64 start = new->ram_base + old->ram_size, size = new->ram_size - old->ram_size;
		int r = add_memory(0, start, size, MHP_NONE);

		pr_info("machrev: memory %llu -> %llu MB (%llu MB added at 0x%llx%s)\n",
			old->ram_size >> 20, new->ram_size >> 20, size >> 20, start,
			r ? ", add_memory failed" : "");
		if (!r)
			adopted |= REV_MEMORY | REV_DEVICES;
	} else if (new->ram_size < old->ram_size) {
		pr_info("machrev: memory %llu -> %llu MB: a smaller node is not taken (the balloon is the way down)\n",
			old->ram_size >> 20, new->ram_size >> 20);
	}
	/* F7/F8: harts came or went. Up: cpu_up (SBI HSM start, our own entry). Down: cpu_down (HSM stop). */
	if (new->online != old->online || new->ncpu != old->ncpu) {
		int up = 0, down = 0, r;

		for (i = 0; i < new->ncpu; i++) {
			int cpu = cpu_of_hart(new->hartid[i]);

			if (cpu < 0) {
				pr_info("machrev: hart %u is no cpu of this kernel\n", new->hartid[i]);
				continue;
			}
			if (!new->parked[i] && !cpu_online(cpu)) {
				r = add_cpu(cpu);
				if (r == 0)
					up++;
				else
					pr_info("machrev: cpu %d (hart %u) would not come up: %d\n", cpu, new->hartid[i], r);
			} else if (new->parked[i] && cpu_online(cpu) && cpu != 0) {
				r = remove_cpu(cpu);
				if (r == 0)
					down++;
				else
					pr_info("machrev: cpu %d (hart %u) would not go down: %d\n", cpu, new->hartid[i], r);
			}
		}
		pr_info("machrev: cpus %d -> %d (%d up, %d down)\n", old->online, new->online, up, down);
		adopted |= REV_DEVICES;
	}
	/* F5/F6: the timer's source and rate, through the one patch the timer needs (0001) */
	if (new->sstc != old->sstc || (new->timebase && new->timebase != old->timebase)) {
		riscv_timer_revise(new->sstc, new->timebase);
		pr_info("machrev: timer via %s at %u Hz\n", new->sstc ? "stimecmp" : "sbi", new->timebase);
		adopted |= REV_TIMER;
	}
	/* what a stock kernel keeps in a place this module cannot reach */
	for (i = 0; i < new->ncpu && i < old->ncpu; i++)
		if (new->hartid[i] != old->hartid[i]) {
			pr_info("machrev: harts renumbered: not taken by this kernel\n");
			break;
		}
	for (i = 0; i < new->nvirtio && i < old->nvirtio; i++)
		if (new->virtio_base[i] != old->virtio_base[i] || new->virtio_irq[i] != old->virtio_irq[i]) {
			pr_info("machrev: a virtio device moved (0x%llx irq %u -> 0x%llx irq %u): not taken by this kernel\n",
				old->virtio_base[i], old->virtio_irq[i], new->virtio_base[i], new->virtio_irq[i]);
			break;
		}
	if (new->rtc != old->rtc) {
		if (new->rtc)
			rtc_back(new->rtc_base, new->rtc_irq);
		else
			rtc_gone();
		adopted |= REV_DEVICES;
	}
	if ((what & REV_INTC) && new->intc && old->intc && strcmp(new->intc, old->intc))
		pr_info("machrev: interrupt controller %s -> %s: Linux keeps its own (legacy tier)\n", old->intc, new->intc);
	/* F22: the SBI extension set. Linux keeps one decision from its boot-time
	 * probe: remote fences through SBI RFENCE (a static key) or through IPIs.
	 * The key is exported; the probe is repeated and the key follows. */
	if ((what & REV_SBI)) {
		bool rf = sbi_probe_extension(SBI_EXT_RFENCE) > 0;
		bool had = static_branch_likely(&riscv_sbi_for_rfence);

		if (rf && !had)
			static_branch_enable(&riscv_sbi_for_rfence);
		else if (!rf && had)
			static_branch_disable(&riscv_sbi_for_rfence);
		pr_info("machrev: sbi extensions re-probed: remote fences by %s%s\n",
			rf ? "SBI RFENCE" : "IPI", rf == had ? " (as before)" : "");
		adopted |= REV_SBI;
	}
	if ((what & REV_VLEN))
		pr_info("machrev: the vector unit changed: not taken by this kernel\n");
	return adopted;
}

/* one revision: read the tree the host rewrote, take it, acknowledge */
static void take_revision(struct machrev *m)
{
	struct sbiret r;
	unsigned long gen, gpa, len, what;
	void *blob, *mem;
	struct device_node *root = NULL;
	struct facts fresh;
	u32 adopted;

	r = mach(MACH_PENDING, 0);
	gen = r.value;
	if (r.error || gen == m->gen)
		return;
	what = mach(MACH_WHAT, 0).value;
	gpa = mach(MACH_DESCRIBE, 0).value;
	len = mach(MACH_DTB_MAX, 0).value;
	blob = memremap(gpa, len, MEMREMAP_WB);
	if (!blob) {
		pr_err("machrev: cannot map the description at 0x%lx\n", gpa);
		return;
	}
	mem = of_fdt_unflatten_tree((const unsigned long *)blob, NULL, &root);
	if (!mem || !root) {
		pr_err("machrev: the description at 0x%lx does not parse\n", gpa);
		memunmap(blob);
		return;
	}
	read_facts(root, &fresh);
	adopted = apply(m, &fresh, what);
	m->cur = fresh;
	m->gen = gen;
	m->revisions++;
	m->adopted_total |= adopted;
	mach(MACH_ACK, adopted);
	pr_info("machrev: revision %lu (what 0x%lx): adopted 0x%x\n", gen, what, adopted);
	kfree(mem);
	memunmap(blob);
}

static irqreturn_t machrev_hard(int irq, void *p)
{
	struct machrev *m = p;

	writel(1, m->regs + MR_ACK);	/* the line down; the work in the thread */
	return IRQ_WAKE_THREAD;
}

static irqreturn_t machrev_thread(int irq, void *p)
{
	take_revision(p);
	return IRQ_HANDLED;
}

static ssize_t revisions_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct machrev *m = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu %lu 0x%lx\n", m->gen, m->revisions, m->adopted_total);
}
static DEVICE_ATTR_RO(revisions);

static int machrev_probe(struct platform_device *pdev)
{
	struct machrev *m;
	struct sbiret r;
	int err;

	m = devm_kzalloc(&pdev->dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = &pdev->dev;
	m->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(m->regs))
		return PTR_ERR(m->regs);
	m->irq = platform_get_irq(pdev, 0);
	if (m->irq < 0)
		return m->irq;
	r = mach(MACH_VERSION, 0);
	if (r.error) {
		dev_err(&pdev->dev, "the host does not speak revision (%ld)\n", r.error);
		return -ENODEV;
	}
	read_facts(of_root, &m->cur);
	m->gen = mach(MACH_PENDING, 0).value;	/* what the tree we booted from is */
	dev_set_drvdata(&pdev->dev, m);
	err = devm_request_threaded_irq(&pdev->dev, m->irq, machrev_hard, machrev_thread,
					IRQF_ONESHOT, "machrev", m);
	if (err)
		return err;
	device_create_file(&pdev->dev, &dev_attr_revisions);
	pr_info("machrev: speaking revision to the host (protocol %lu): %d cpu(s) (%d online), %llu MB, cache block %u, %s\n",
		r.value, m->cur.ncpu, m->cur.online, m->cur.ram_size >> 20, m->cur.cboz,
		m->cur.intc ? m->cur.intc : "?");
	/* a revision owed before this driver was here (a machine that arrived from elsewhere) */
	if (readl(m->regs + MR_STATUS) & 1) {
		writel(1, m->regs + MR_ACK);
		take_revision(m);
	}
	return 0;
}

static const struct of_device_id machrev_of_match[] = {
	{ .compatible = "xv6h,machrev" },
	{ }
};
MODULE_DEVICE_TABLE(of, machrev_of_match);

static struct platform_driver machrev_driver = {
	.probe = machrev_probe,
	.driver = {
		.name = "machrev",
		.of_match_table = machrev_of_match,
	},
};
module_platform_driver(machrev_driver);

MODULE_DESCRIPTION("xv6H machine-description revisions");
MODULE_LICENSE("GPL");
