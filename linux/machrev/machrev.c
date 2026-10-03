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
#include <linux/timekeeping.h>
#include <linux/version.h>
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
#include <linux/irq.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_net.h>
#include <asm/sbi.h>
#include <asm/cacheflush.h>
#include <asm/vector.h>
#include <linux/wait.h>
#include <linux/irqchip/xv6h-vic.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

/* linux/patches/0001-riscv-timer-revise.patch */
extern void riscv_timer_revise(bool sstc, u32 hz, void (*now)(void));
/* linux/patches/0002-virtio-mmio-relocate.patch */
extern int virtio_mmio_relocate(struct platform_device *pdev, resource_size_t base, int virq);
extern struct virtio_device *virtio_mmio_vdev(struct platform_device *pdev);
/* linux/patches/0003-riscv-rebind-hart.patch */
extern int riscv_rebind_hart(unsigned int cpu, unsigned long hartid);
/* linux/patches/0004-riscv-vector-revise.patch: riscv_v_revise(), riscv_v_withdrawn, riscv_v_request_hook (asm/vector.h) */

#define SBI_EXT_MACH 0x4D414348
#define MACH_VERSION 0
#define MACH_PENDING 1
#define MACH_DESCRIBE 2
#define MACH_ACK 3
#define MACH_DTB_MAX 4
#define MACH_REQUEST 5
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
	bool vec;		/* the vector unit is described (F10) */
	int nvirtio;
	u64 virtio_base[MAX_VIRTIO];
	u32 virtio_irq[MAX_VIRTIO];
	u32 virtio_irq_type[MAX_VIRTIO];	/* the second cell where the controller has two (APLIC); 0: level-high */
	bool rtc;
	u64 rtc_base;
	u32 rtc_irq;
	const char *intc;
	int vic_kind;			/* the description-bound controller (patch 0005): 0 plic, 1 aplic, 2 imsic; -1 none */
	u64 intc_base, intc_size;
};

struct machrev {
	struct device *dev;
	void __iomem *regs;
	int irq;
	struct facts cur;
	unsigned long gen;
	unsigned long revisions, adopted_total;
	struct mutex lock;		/* one revision at a time: the anchor's thread and the poll */
	struct delayed_work poll;	/* F1: the generation polled, because the anchor's line cannot announce the exchange of the controller that carries it */
	unsigned long polled;
};

static struct sbiret mach(unsigned long fid, unsigned long a0)
{
	return sbi_ecall(SBI_EXT_MACH, fid, a0, 0, 0, 0, 0, 0);
}

static struct sbiret mach2(unsigned long fid, unsigned long a0, unsigned long a1)
{
	return sbi_ecall(SBI_EXT_MACH, fid, a0, a1, 0, 0, 0, 0);
}

/*
 * F6: the host serves the described rate from this call. riscv_timer_revise() makes it inside the
 * stop_machine that re-bases the timekeeper, so no CPU reads the clock between the counter's
 * change and the timekeeper's. A host that does not know the call (it answers "not supported")
 * changes the rate at the acknowledgement instead.
 */
#define MACH_RATE_NOW 7
static void machrev_rate_now(void)
{
	mach(MACH_RATE_NOW, 0);
}

/* what the tree says the machine is; the same reading for the live tree and a revised one */
/* one node of the description: a device, the RTC or an interrupt controller */
static void read_node(struct device_node *c, struct facts *f)
{
	u64 reg[2];

	if (of_device_is_compatible(c, "virtio,mmio") && f->nvirtio < MAX_VIRTIO) {
		if (of_property_read_variable_u64_array(c, "reg", reg, 2, 2) == 2)
			f->virtio_base[f->nvirtio] = reg[0];
		of_property_read_u32_index(c, "interrupts", 0, &f->virtio_irq[f->nvirtio]);
		f->virtio_irq_type[f->nvirtio] = 0;
		if (of_property_count_u32_elems(c, "interrupts") >= 2)
			of_property_read_u32_index(c, "interrupts", 1, &f->virtio_irq_type[f->nvirtio]);
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
	} else if (of_device_is_compatible(c, "xv6h,vic")) {
		/* the description-bound controller: its kind is a property, its window the reg */
		const char *k = NULL;

		of_property_read_string(c, "xv6h,kind", &k);
		f->vic_kind = !k ? -1 : !strcmp(k, "plic") ? XV6H_VIC_PLIC : !strcmp(k, "aplic") ? XV6H_VIC_APLIC :
			      !strcmp(k, "imsic") ? XV6H_VIC_IMSIC : -1;
		f->intc = f->vic_kind == XV6H_VIC_PLIC ? "plic" : f->vic_kind == XV6H_VIC_APLIC ? "aplic" :
			  f->vic_kind == XV6H_VIC_IMSIC ? "imsic" : NULL;
		if (of_property_read_variable_u64_array(c, "reg", reg, 2, 2) == 2) {
			f->intc_base = reg[0];
			f->intc_size = reg[1];
		}
	}
}

static void read_facts(struct device_node *root, struct facts *f)
{
	struct device_node *n, *cpus;
	u64 reg[2];

	memset(f, 0, sizeof(*f));
	f->vic_kind = -1;
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
			if (!of_property_read_string(n, "riscv,isa", &isa) && isa) {
				const char *p;

				f->sstc = strstr(isa, "sstc") != NULL;
				/* the single-letter extensions after rv64, up to the first '_' */
				for (p = isa + 4; *p && *p != '_'; p++)
					if (*p == 'v')
						f->vec = true;
			}
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
	/*
	 * the devices live on a bus node, and the interrupt controllers on the bus or at the
	 * root: xv6H and QEMU's virt put them all under /soc, kvmtool puts the devices under /smb
	 * and the controllers at the root. The module first looked for /soc by name and, under
	 * kvmtool, found no devices (the KVM port's first device-map revision was "adopted 0x0")
	 * and no controller; a bus is a "simple-bus", whatever it is called.
	 */
	for_each_child_of_node(root, n) {
		struct device_node *c;

		if (of_node_name_eq(n, "soc") || of_device_is_compatible(n, "simple-bus")) {
			for_each_child_of_node(n, c)
				read_node(c, f);
		} else {
			read_node(n, f);
		}
	}
}

/*
 * the logical cpu that is hart `hart' (cpu numbering is Linux's own). The
 * map is the module's: the live tree's cpu nodes keep the reg they booted
 * with, so after a renumbering a lookup by the tree finds the old numbers
 * only -- a renumbering *back* to them was then "no cpu of this kernel" and
 * silently not applied, and every SBI call the kernel made afterwards named
 * harts the machine no longer had (lxsbi: -EINVAL on each, IPIs lost, the
 * SBI-set revision four seconds).
 */
static u32 hart_of_cpu[NR_CPUS];
static bool hart_map_ready;
static DECLARE_WAIT_QUEUE_HEAD(vec_back);	/* F12: the unit came back */

static void hart_map_init(void)
{
	int cpu;

	if (hart_map_ready)
		return;
	for_each_possible_cpu(cpu) {
		struct device_node *n = of_get_cpu_node(cpu, NULL);
		u32 hid = ~0u;

		if (!n || of_property_read_u32(n, "reg", &hid))
			hid = ~0u;
		hart_of_cpu[cpu] = hid;
		of_node_put(n);
	}
	hart_map_ready = true;
}

static int cpu_of_hart(u32 hart)
{
	int cpu;

	hart_map_init();
	for_each_possible_cpu(cpu)
		if (hart_of_cpu[cpu] == hart)
			return cpu;
	return -1;
}

static int __maybe_unused cpu_of_hart_by_tree(u32 hart)	/* the boot tree's view, kept for reference */
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

/* a wired line of the machine, as a Linux irq, mapped on the live tree's controller */
static int map_line_type(u32 hwirq, u32 type)
{
	struct device_node *ic;
	struct irq_fwspec fwspec = { };
	int virq;

	/* the description-bound controller first (patch 0005): when the tree has one, every
	 * device line hangs from it, whatever the backend (KVM's first vic tree: every move
	 * "not moved", because only the PLIC and APLIC compatibles were looked for) */
	ic = of_find_compatible_node(NULL, NULL, "xv6h,vic");
	if (!ic)
		ic = of_find_compatible_node(NULL, NULL, "riscv,plic0");
	if (!ic)
		ic = of_find_compatible_node(NULL, NULL, "sifive,plic-1.0.0");
	if (!ic)
		ic = of_find_compatible_node(NULL, NULL, "riscv,aplic");
	if (!ic)
		return -ENODEV;
	fwspec.fwnode = of_node_to_fwnode(ic);
	fwspec.param[0] = hwirq;
	fwspec.param[1] = type ? type : IRQ_TYPE_LEVEL_HIGH;
	fwspec.param_count = of_device_is_compatible(ic, "riscv,aplic") ? 2 : 1;
	/*
	 * A line already mapped is taken as it is mapped. Asking for it again with
	 * another trigger is refused ("type mismatch, failed to map hwirq"): kvmtool
	 * describes its virtio lines as edge-rising, and every move under KVM was
	 * "not moved" until this. A line not yet mapped is asked for level-high (a
	 * virtio-mmio device holds its line until the status is acknowledged, so a
	 * level is right whatever the tree said; an edge source raised as a level
	 * is seen once per edge).
	 */
	{
		struct irq_domain *d = irq_find_matching_fwnode(fwspec.fwnode, DOMAIN_BUS_ANY);

		virq = d ? irq_find_mapping(d, hwirq) : 0;
		if (virq > 0) {
			of_node_put(ic);
			return virq;
		}
	}
	virq = irq_create_fwspec_mapping(&fwspec);
	of_node_put(ic);
	return virq > 0 ? virq : -ENODEV;
}

static int map_line(u32 hwirq)
{
	return map_line_type(hwirq, 0);
}

/* F3/F15: a virtio device's window or line moved; the driver follows (patch 0002) */
/*
 * a virtio-mmio device is one whose node says so or that the virtio-mmio driver
 * holds, not one whose name has "virtio_mmio" in it: the name is the boot node's,
 * "virtio_mmio@" under xv6H and QEMU but "virtio@" under kvmtool, where every
 * move was "no virtio_mmio platform device" (the KVM port's first device map)
 */
static bool is_virtio_mmio(struct device *dev)
{
	return (dev->of_node && of_device_is_compatible(dev->of_node, "virtio,mmio")) ||
	       (dev->driver && !strcmp(dev->driver->name, "virtio-mmio"));
}

static int virtio_at(struct device *dev, const void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	return r && r->start == *(const u64 *)data && is_virtio_mmio(dev);
}

static int virtio_moved(u64 old_base, u64 new_base, u32 new_irq, u32 type)
{
	struct device *dev;
	int virq, rc;

	/* by where the device is now, not by its boot name: a device moved once
	 * keeps the name of its boot window (relocate updates the resource, the
	 * platform bus does not rename), so a second move by name found nothing
	 * and Linux then read the vanished window (d2e: Oops at 0x10103060) */
	dev = bus_find_device(&platform_bus_type, NULL, &old_base, virtio_at);
	if (!dev) {
		pr_info("machrev: no virtio_mmio platform device at 0x%llx to move\n", old_base);
		return -ENODEV;
	}
	virq = map_line_type(new_irq, type);
	rc = virq < 0 ? virq : virtio_mmio_relocate(to_platform_device(dev), new_base, virq);
	put_device(dev);
	pr_info("machrev: virtio device at 0x%llx -> 0x%llx line %u: %s\n", old_base, new_base, new_irq,
		rc ? "not moved" : "moved");
	return rc;
}

static void rtc_back(u64 base, u32 hwirq)
{
	struct timespec64 wall0, mono0, wall1, mono1, want, moved;
	struct resource res[2];
	int virq = map_line(hwirq);

	if (virq < 0) {
		pr_info("machrev: rtc back, but no interrupt controller node to map its line on\n");
		return;
	}
	memset(res, 0, sizeof(res));
	res[0] = DEFINE_RES_MEM(base, 0x1000);
	res[1] = DEFINE_RES_IRQ(virq);
	/* Registering an RTC runs the core's hctosys and sets the wall clock from
	 * it, to the second (defect 28: every renumbering that moved the RTC's line
	 * moved CLOCK_REALTIME against CLOCK_MONOTONIC by up to a second, plus
	 * whatever the host's clock and the guest's disagreed by across a move).
	 * The wall clock was right before; it stays where its own monotonic
	 * clock says it should be. */
	ktime_get_real_ts64(&wall0);
	ktime_get_ts64(&mono0);
	machrev_rtc = platform_device_register_resndata(NULL, "goldfish_rtc", -1, res, 2, NULL, 0);
	if (IS_ERR(machrev_rtc)) {
		pr_info("machrev: rtc back at 0x%llx, registering it failed: %ld\n", base, PTR_ERR(machrev_rtc));
		machrev_rtc = NULL;
		return;
	}
	ktime_get_real_ts64(&wall1);
	ktime_get_ts64(&mono1);
	want = timespec64_add(wall0, timespec64_sub(mono1, mono0));
	moved = timespec64_sub(wall1, want);
	if (moved.tv_sec != 0 || moved.tv_nsec > 10000000 || moved.tv_nsec < -10000000) {
		do_settimeofday64(&want);
		pr_info("machrev: the rtc's registration moved the wall clock by %lld ms: put back\n",
			(long long)(moved.tv_sec * 1000 + moved.tv_nsec / 1000000));
	}
	pr_info("machrev: rtc back at 0x%llx line %u (irq %d): goldfish_rtc registered\n", base, hwirq, virq);
}

/*
 * F19: the NIC's identity. The host tells it through the device -- a
 * configuration change, the MAC re-read -- which Linux's virtio-net driver
 * takes at probe only. So on every revision the network device's configured
 * MAC is compared with what the device now says, and the stack's address is
 * set to follow (the same call `ip link set address' makes).
 */
static int nic_follow(const struct facts *f)
{
	int i, changed = 0;

	for (i = 0; i < f->nvirtio; i++) {
		struct device *dev;
		struct virtio_device *vdev;
		struct net_device *nd;
		u8 mac[ETH_ALEN];

		dev = bus_find_device(&platform_bus_type, NULL, &f->virtio_base[i], virtio_at);
		if (!dev)
			continue;
		vdev = virtio_mmio_vdev(to_platform_device(dev));
		put_device(dev);
		if (!vdev || vdev->id.device != VIRTIO_ID_NET || !virtio_has_feature(vdev, VIRTIO_NET_F_MAC))
			continue;
		virtio_cread_bytes(vdev, offsetof(struct virtio_net_config, mac), mac, ETH_ALEN);
		nd = dev_get_by_name(&init_net, "eth0");
		if (!nd)
			continue;
		if (!ether_addr_equal(nd->dev_addr, mac)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
			struct sockaddr_storage sa;	/* 6.16 widened the address argument */
#else
			struct sockaddr sa;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
			sa.ss_family = ARPHRD_ETHER;
			memcpy(sa.__data, mac, ETH_ALEN);
#else
			sa.sa_family = ARPHRD_ETHER;
			memcpy(sa.sa_data, mac, ETH_ALEN);
#endif
			rtnl_lock();
			dev_set_mac_address(nd, &sa, NULL);
			rtnl_unlock();
			pr_info("machrev: nic identity -> %pM (the device said so)\n", mac);
			changed = 1;
		}
		dev_put(nd);
	}
	return changed;
}

/*
 * F18 (and any device that comes or goes): a virtio-mmio node in the new
 * tree that was not in the old one is a device the machine now has -- a
 * virtio-console, say. Linux made its platform devices from the boot tree;
 * this makes the new one by hand (its window and line as the tree says), and
 * the virtio-mmio driver probes it, and the class driver behind that. One
 * that went is unregistered. The kernel's own console stays where it was
 * booted (`console='), which is Linux's rule, not ours: what changes is
 * which consoles the machine has.
 */
static struct platform_device *machrev_vdev[MAX_VIRTIO];
static int machrev_nvdev;

static void virtio_appeared(u64 base, u32 hwirq, u32 type)
{
	struct resource res[2];
	struct platform_device *pdev;
	int virq = map_line_type(hwirq, type);

	if (virq < 0 || machrev_nvdev >= MAX_VIRTIO) {
		pr_info("machrev: a virtio device at 0x%llx appeared, and could not be made\n", base);
		return;
	}
	memset(res, 0, sizeof(res));
	res[0] = DEFINE_RES_MEM(base, 0x200);
	res[1] = DEFINE_RES_IRQ(virq);
	pdev = platform_device_register_resndata(NULL, "virtio-mmio", -1, res, 2, NULL, 0);
	if (IS_ERR(pdev)) {
		pr_info("machrev: a virtio device at 0x%llx appeared; registering it failed: %ld\n", base, PTR_ERR(pdev));
		return;
	}
	machrev_vdev[machrev_nvdev++] = pdev;
	pr_info("machrev: virtio device at 0x%llx line %u appeared: registered\n", base, hwirq);
}

static void virtio_went(u64 base)
{
	struct device *dev;
	int i;

	for (i = 0; i < machrev_nvdev; i++) {
		struct resource *r = platform_get_resource(machrev_vdev[i], IORESOURCE_MEM, 0);

		if (r && r->start == base) {
			platform_device_unregister(machrev_vdev[i]);
			machrev_vdev[i] = machrev_vdev[--machrev_nvdev];
			pr_info("machrev: virtio device at 0x%llx went: unregistered\n", base);
			return;
		}
	}
	dev = bus_find_device(&platform_bus_type, NULL, &base, virtio_at);
	if (dev) {
		platform_device_unregister(to_platform_device(dev));
		put_device(dev);
		pr_info("machrev: virtio device at 0x%llx went: the boot tree's unregistered\n", base);
	}
}

static bool has_virtio(const struct facts *f, u64 base)
{
	int i;

	for (i = 0; i < f->nvirtio; i++)
		if (f->virtio_base[i] == base)
			return true;
	return false;
}

/* what this kernel can take, taken; what it cannot, said */
static u32 apply(struct machrev *m, struct facts *new, u32 what)
{
	bool renumbered = false;
	const struct facts *old = &m->cur;
	u32 adopted = 0;
	int i;

	/* The order matters (defect 25, found by eight facts at once): first the two
	 * facts that change how every later step waits and flushes. The SBI set,
	 * because add_memory and cpu_down flush TLBs, and a flush through an
	 * extension this host withholds fails (-ENOTSUPP) until the key follows;
	 * the timer and timebase, because a delay or a timeout runs at the rate the
	 * kernel believes, and a hart parking that waits at a tenth of the rate
	 * waits ten times (the crossing adopted after 72-114 s, alone each fact
	 * in under 100 ms). Then everything else. */
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
	/* ... then the harts' numbers (F9), before anything sends an IPI by hart id: the
	 * timer's re-rate cross-calls every cpu, and a cross-call addressed to the old
	 * numbers never returns (the grand tour's hop C->A: "__sbi_rfence_v02_call:
	 * hbase = [4] ... failed (error [-22])", a soft lockup in riscv_timer_revise) */
	/* F9: the harts were renumbered. cpu i was hart old[i]; it is hart new[i] now. */
	for (i = 0; i < new->ncpu && i < old->ncpu; i++)
		if (new->hartid[i] != old->hartid[i]) {
			int cpu = cpu_of_hart(old->hartid[i]);

			int rc;

			if (cpu < 0) {
				pr_info("machrev: hart %u is no cpu of this kernel: not renumbered\n", old->hartid[i]);
				continue;
			}
			rc = riscv_rebind_hart(cpu, new->hartid[i]);
			hart_of_cpu[cpu] = new->hartid[i];	/* the kernel's map changed whatever its controllers said */
			adopted |= REV_DEVICES;
			renumbered = true;
			pr_info("machrev: cpu %d: hart %u -> %u%s\n", cpu, old->hartid[i], new->hartid[i],
				rc ? " (a controller did not follow)" : "");
		}
	if (renumbered && new->vic_kind >= 0)
		xv6h_vic_rehart();		/* contexts, IDCs and targets are by hart id */
	/*
	 * The RTC leaves before the virtio devices move and comes back after
	 * them: on a renumbering the line it leaves may be the one a virtio
	 * device moves to (lxirq), and the line it moves to may be the one a
	 * virtio device is still on -- registering it there fails with
	 * "genirq: Flags mismatch irq 16 (goldfish_rtc) vs (virtio0)" and the
	 * machine has no clock until the next revision (hunt4c, every arrival
	 * on the default-numbered host from crossing ~160 on).
	 */
	bool rtc_returns = false;

	/* F5/F6: the timer's source and rate, through the one patch the timer needs (0001) */
	if (new->sstc != old->sstc || (new->timebase && new->timebase != old->timebase)) {
		riscv_timer_revise(new->sstc, new->timebase, machrev_rate_now);
		pr_info("machrev: timer via %s at %u Hz\n", new->sstc ? "stimecmp" : "sbi", new->timebase);
		adopted |= REV_TIMER;
	}

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
	/* F18 and friends: virtio devices that came or went (see above) */
	if (new->nvirtio != old->nvirtio) {
		for (i = 0; i < new->nvirtio; i++)
			if (!has_virtio(old, new->virtio_base[i])) {
				virtio_appeared(new->virtio_base[i], new->virtio_irq[i], new->virtio_irq_type[i]);
				adopted |= REV_DEVICES;
			}
		for (i = 0; i < old->nvirtio; i++)
			if (!has_virtio(new, old->virtio_base[i])) {
				virtio_went(old->virtio_base[i]);
				adopted |= REV_DEVICES;
			}
	}

	/* F19: told through the device; looked at on every revision */
	if (nic_follow(new))
		adopted |= REV_DEVICES;

	/* what a stock kernel keeps in a place this module cannot reach */
	if (new->rtc != old->rtc) {
		if (new->rtc)
			rtc_returns = true;
		else
			rtc_gone();
		adopted |= REV_DEVICES;
	} else if (new->rtc && (new->rtc_irq != old->rtc_irq || new->rtc_base != old->rtc_base)) {
		/* F3/F15: the same clock on another line or at another address */
		rtc_gone();
		rtc_returns = true;
		adopted |= REV_DEVICES;
	}
	/*
	 * The same number of devices, in the tree's order: differences are moves
	 * (F3/F15 -- a new map moves every window, so no new address is in the
	 * old set; treating that as "all went, all appeared" unregistered the
	 * disk under the running system: m7lx layout 0/20). A different number:
	 * devices came or went (F18), matched by address below.
	 */
	if (new->nvirtio == old->nvirtio) {
		for (i = 0; i < new->nvirtio; i++)
			if (new->virtio_base[i] != old->virtio_base[i] || new->virtio_irq[i] != old->virtio_irq[i]) {
				if (virtio_moved(old->virtio_base[i], new->virtio_base[i], new->virtio_irq[i], new->virtio_irq_type[i]) == 0)
					adopted |= REV_DEVICES;
			}
	}
	if (rtc_returns)
		rtc_back(new->rtc_base, new->rtc_irq);
	/*
	 * F1/F2: the interrupt controller's kind or address. With the description-
	 * bound controller (patch 0005) the driver re-binds: the new backend is
	 * brought up with every line as this kernel last asked for it, on every
	 * hart, before the acknowledgement. A kernel on the stock drivers keeps
	 * its own and says so; the host serves it from the legacy tier.
	 */
	if ((what & REV_INTC) && new->vic_kind >= 0 &&
	    (new->vic_kind != old->vic_kind || new->intc_base != old->intc_base)) {
		int rc = xv6h_vic_rebind(new->vic_kind, new->intc_base, new->intc_size);

		pr_info("machrev: interrupt controller %s at 0x%llx -> %s at 0x%llx: %s\n",
			old->intc ? old->intc : "?", old->intc_base, new->intc ? new->intc : "?", new->intc_base,
			rc ? "not taken" : "rebound");
		if (!rc)
			adopted |= REV_INTC;
	} else if ((what & REV_INTC) && new->intc && old->intc && strcmp(new->intc, old->intc))
		pr_info("machrev: interrupt controller %s -> %s: Linux keeps its own (legacy tier)\n", old->intc, new->intc);
	/*
	 * F10/F11: the vector unit taken away or given back (patch 0004). Away:
	 * ceded if no task holds vector state, refused with the count if one does
	 * -- the host then keeps the description as it was (the unit stays) and
	 * the operator's move is refused with that reason. Back: taken, with the
	 * width the hart now reports.
	 */
	if ((what & REV_VLEN) && new->vec != old->vec) {
		if (!new->vec) {
			int held = riscv_v_revise(false);

			if (held > 0) {
				pr_info("machrev: keeping the vector unit, %d task(s) hold vector state\n", held);
				new->vec = true;	/* what this kernel actually has */
			} else if (held < 0) {
				pr_info("machrev: the vector unit cannot be withdrawn here (%d)\n", held);
				new->vec = true;
			} else {
				pr_info("machrev: vector unit withdrawn: no task held state\n");
				adopted |= REV_VLEN;
			}
		} else {
			int rc = riscv_v_revise(true);

			pr_info("machrev: vector unit back%s\n", rc ? ": not taken" : "");
			if (!rc) {
				adopted |= REV_VLEN;
				wake_up_all(&vec_back);
			}
		}
	}
	return adopted;
}

/*
 * F12: a task met the withdrawn unit (the kernel's first-use trap, patch 0004)
 * and asks the machine for it. The host grants it if it was only hidden
 * (`lazyvector', or a machine that arrived at a host that has one), and the
 * revision that brings it back comes on the anchor's line; the task waits for
 * it to be taken, then retries its instruction. Refused, the task gets its
 * illegal instruction.
 */
static int machrev_ask_vector(void)
{
	struct sbiret r = mach(MACH_REQUEST, REV_VLEN);

	if (r.error) {
		pr_info_ratelimited("machrev: asked the host for the vector unit: refused (%ld)\n", r.error);
		return -ENODEV;
	}
	if (!wait_event_timeout(vec_back, !READ_ONCE(riscv_v_withdrawn), HZ)) {
		pr_info("machrev: asked the host for the vector unit: granted, but the revision did not come\n");
		return -ETIMEDOUT;
	}
	pr_info("machrev: asked the host for the vector unit: granted\n");
	return 0;
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

	mutex_lock(&m->lock);
	r = mach(MACH_PENDING, 0);
	gen = r.value;
	if (r.error || gen == m->gen) {
		mutex_unlock(&m->lock);
		return;
	}
	what = mach(MACH_WHAT, 0).value;
	gpa = mach(MACH_DESCRIBE, 0).value;
	len = mach(MACH_DTB_MAX, 0).value;
	blob = memremap(gpa, len, MEMREMAP_WB);
	if (!blob) {
		pr_err("machrev: cannot map the description at 0x%lx\n", gpa);
		mutex_unlock(&m->lock);
		return;
	}
	mem = of_fdt_unflatten_tree((const unsigned long *)blob, NULL, &root);
	if (!mem || !root) {
		pr_err("machrev: the description at 0x%lx does not parse\n", gpa);
		memunmap(blob);
		mutex_unlock(&m->lock);
		return;
	}
	read_facts(root, &fresh);
	adopted = apply(m, &fresh, what);
	m->cur = fresh;
	m->gen = gen;
	m->revisions++;
	m->adopted_total |= adopted;
	mach2(MACH_ACK, adopted, gen);	/* named: a stale ack is refused (defect 20) */
	pr_info("machrev: revision %lu (what 0x%lx): adopted 0x%x\n", gen, what, adopted);
	kfree(mem);
	memunmap(blob);
	mutex_unlock(&m->lock);
}

/*
 * F1's channel problem. Every revision is announced on the anchor's line,
 * which is a device line through the interrupt controller -- and the one
 * revision that exchanges the controller cannot be announced by a line that
 * goes through it (lxintc4: "described no", both vcpus idle, the line raised
 * on a controller the guest's driver had not yet bound). Theorem 2 asks for a
 * channel independent of what the revision changes; for the controller
 * itself that channel is the generation counter, which the guest reads with
 * one SBI call. With the description-bound controller in use, the driver
 * polls it every 50 ms; the anchor's line stays the fast path for everything
 * else, and the poll is what makes the exchange of the controller a revision
 * like the others.
 */
static void machrev_poll(struct work_struct *w)
{
	struct machrev *m = container_of(to_delayed_work(w), struct machrev, poll);

	m->polled++;
	if (mach(MACH_PENDING, 0).value != m->gen)
		take_revision(m);
	schedule_delayed_work(&m->poll, msecs_to_jiffies(50));
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
	mutex_init(&m->lock);
	INIT_DELAYED_WORK(&m->poll, machrev_poll);
	err = devm_request_threaded_irq(&pdev->dev, m->irq, machrev_hard, machrev_thread,
					IRQF_ONESHOT, "machrev", m);
	if (err)
		return err;
	if (m->cur.vic_kind >= 0) {
		schedule_delayed_work(&m->poll, msecs_to_jiffies(50));
		pr_info("machrev: the controller is revisable: polling the generation every 50 ms as well\n");
	}
	device_create_file(&pdev->dev, &dev_attr_revisions);
	riscv_v_request_hook = machrev_ask_vector;	/* F12 */
	pr_info("machrev: speaking revision to the host (protocol %lu): %d cpu(s) (%d online), %llu MB, cache block %u, %s, vector unit %s\n",
		r.value, m->cur.ncpu, m->cur.online, m->cur.ram_size >> 20, m->cur.cboz,
		m->cur.intc ? m->cur.intc : "?", m->cur.vec ? "present" : "absent");
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
