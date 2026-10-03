/*
 * Migration for kvmtool on RISC-V (A.6): kvmtool has none. A running VM is moved to another
 * kvmtool on another KVM host:
 *
 *   destination:  lkvm run ... --revise --incoming PORT       (same disk, memory and cpus)
 *   source:       lkvm migrate -n VM --to ADDR:PORT
 *
 * Stop and copy. The source pauses its vcpus, lets the disk go quiet, and sends, in order:
 * the guest's RAM; the disk's contents; every vcpu register KVM lists (KVM_GET_REG_LIST, the
 * timer's included, so the clock is the guest's own across the move); the in-kernel AIA's
 * APLIC and IMSIC registers (device attributes); the PLIC model's state; every virtio-mmio
 * device (transport registers, negotiated features and status, each queue's rings and
 * indices, its window and line); the 8250s; the revision state (generation, slots, what the
 * description says). The destination applies all of it before its vcpus first run, notifies
 * every queue once (requests the guest posted that the source had not taken are still in the
 * rings, in guest RAM), and then -- the arrival rule -- asks for the revisions its own shape
 * needs (XV6H_ARRIVE="intc plic;timebase 1000000": the destination's facts), as xv6H does on
 * arrival.
 *
 * The configuration itself (ISA and SBI extensions, the number of vcpus, the devices) must be
 * the same on both sides: the ISA, SBI-extension and config registers are not sent.
 */
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/kvm-ipc.h"
#include "kvm/util.h"
#include "kvm/disk-image.h"
#include "kvm/virtio-mmio.h"
#include "kvm/revise.h"
#include "kvm/migrate.h"

#include <linux/kvm.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#define MIG_READY	'R'	/* the destination has the whole machine, applied */
#define MIG_COMMIT	'C'	/* the source lets it go */
#include <unistd.h>

#define MIG_MAGIC 0x31474d4848365658ULL		/* "XV6HHMG1" */

struct mig_hdr {
	u64 magic;
	u32 nrcpus;
	u64 ram_size;
	u64 disk_size;
	u32 nr_virtio;
	u64 t_source_ns;	/* the source's monotonic clock when it stopped (for the downtime) */
};

static u64 now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (u64)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static int xfer_all(int fd, void *buf, size_t n, bool out)
{
	char *p = buf;

	while (n) {
		ssize_t r = out ? write(fd, p, n) : read(fd, p, n);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		n -= r;
	}
	return 0;
}

/*
 * A test of the receiver (M12): XV6H_MIG_CORRUPT="TAG:OFF:VAL" writes the 32-bit VAL at byte OFF
 * of the first record with that tag, and "TAG:len:N" sends that record N bytes short. The
 * destination must refuse the machine, and the machine must run on here. The case is read
 * afresh at each migration, from the variable or from the file XV6H_MIG_CORRUPT_FILE names.
 */
static char corrupt_spec[64];
static int corrupt_done;

static void corrupt_load(void)
{
	const char *e = getenv("XV6H_MIG_CORRUPT"), *f = getenv("XV6H_MIG_CORRUPT_FILE");
	FILE *fp;

	corrupt_spec[0] = 0;
	corrupt_done = 0;
	if (e)
		snprintf(corrupt_spec, sizeof(corrupt_spec), "%s", e);
	else if (f && (fp = fopen(f, "r"))) {
		if (!fgets(corrupt_spec, sizeof(corrupt_spec), fp))
			corrupt_spec[0] = 0;
		fclose(fp);
	}
}

static int put_corrupt(int fd, u32 tag, const void *data, u64 *len, void **copy)
{
	const char *e = corrupt_spec;
	int done = corrupt_done;
	unsigned t, off;
	long val;
	char what[16];

	*copy = NULL;
	if (done || !*e || sscanf(e, "%u:%15[^:]:%ld", &t, what, &val) != 3 || t != tag)
		return 0;
	corrupt_done = 1;
	if (!strcmp(what, "len")) {
		*len = (u64)val < *len ? *len - val : 0;
		fprintf(stderr, "migrate: test: record %u sent %ld bytes short\n", tag, val);
		return 0;
	}
	off = strtoul(what, NULL, 0);
	if (off + 4 > *len || !(*copy = malloc(*len)))
		return 0;
	memcpy(*copy, data, *len);
	memcpy((char *)*copy + off, &(u32){ (u32)val }, 4);
	fprintf(stderr, "migrate: test: record %u, byte %u set to %ld\n", tag, off, val);
	return 0;
}

static int put(int fd, u32 tag, const void *data, u64 len)
{
	void *copy;
	int r;

	put_corrupt(fd, tag, data, &len, &copy);
	if (copy)
		data = copy;
	r = xfer_all(fd, &tag, sizeof(tag), true) || xfer_all(fd, &len, sizeof(len), true) ||
	    (len && xfer_all(fd, (void *)data, len, true)) ? -1 : 0;
	free(copy);
	return r;
}


static int get_hdr(int fd, u32 *tag, u64 *len)
{
	if (xfer_all(fd, tag, sizeof(*tag), false) || xfer_all(fd, len, sizeof(*len), false))
		return -1;
	return 0;
}

/* ---- the vcpus: every register KVM lists, but the configuration ---- */

static bool reg_is_config(u64 id)
{
	u64 type = id & KVM_REG_RISCV_TYPE_MASK;

	return type == KVM_REG_RISCV_CONFIG || type == KVM_REG_RISCV_ISA_EXT || type == KVM_REG_RISCV_SBI_EXT;
}

static int send_vcpu(int fd, struct kvm_cpu *vcpu, int *nregs)
{
	struct kvm_reg_list probe = { .n = 0 }, *list;
	u8 *buf, *p;
	int r;

	ioctl(vcpu->vcpu_fd, KVM_GET_REG_LIST, &probe);		/* -E2BIG, and the count */
	list = calloc(1, sizeof(*list) + probe.n * sizeof(u64));
	if (!list)
		return -1;
	list->n = probe.n;
	if (ioctl(vcpu->vcpu_fd, KVM_GET_REG_LIST, list) < 0) {
		free(list);
		return -1;
	}
	/* [u32 cpu][records: u64 id, then the value's bytes] */
	buf = malloc(2 * sizeof(u32) + list->n * (sizeof(u64) + 256));
	p = buf;
	*(u32 *)p = vcpu->cpu_id;
	p += sizeof(u32);
	{
		/* the power state is not a register: started, or stopped by HSM, it travels too */
		struct kvm_mp_state mp = { 0 };

		ioctl(vcpu->vcpu_fd, KVM_GET_MP_STATE, &mp);
		*(u32 *)p = mp.mp_state;
		p += sizeof(u32);
	}
	*nregs = 0;
	for (u64 i = 0; i < list->n; i++) {
		u64 id = list->reg[i], size = 1ULL << ((id & KVM_REG_SIZE_MASK) >> KVM_REG_SIZE_SHIFT);
		struct kvm_one_reg reg = { .id = id, .addr = (u64)(unsigned long)(p + sizeof(u64)) };

		if (reg_is_config(id) || size > 256)
			continue;
		if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
			continue;
		*(u64 *)p = id;
		p += sizeof(u64) + size;
		(*nregs)++;
	}
	r = put(fd, T_VCPU, buf, p - buf);
	free(buf);
	free(list);
	return r;
}

/*
 * Everything below reads a record the other host sent. The stream is trusted to come from a host
 * of the fleet (the threat model of the paper's security section), but a record is never read
 * past its own length, and an index in it is never used before it is checked: a corrupt or
 * hostile stream ends the arrival, it does not reach this process's memory.
 */
static int apply_vcpu(struct kvm *kvm, u8 *buf, u64 len, int *nset, int *nfail)
{
	u32 cpu;
	u8 *p = buf + 2 * sizeof(u32), *end = buf + len;
	struct kvm_cpu *vcpu;
	struct kvm_mp_state mp;

	if (len < 2 * sizeof(u32))
		return -1;
	cpu = *(u32 *)buf;
	mp.mp_state = ((u32 *)buf)[1];
	if (cpu >= (u32)kvm->nrcpus)
		return -1;
	vcpu = kvm->cpus[cpu];
	if (ioctl(vcpu->vcpu_fd, KVM_SET_MP_STATE, &mp) < 0)
		fprintf(stderr, "migrate: vcpu %u: mp state %u refused: %s\n", cpu, mp.mp_state, strerror(errno));
	while (p + sizeof(u64) <= end) {
		u64 id = *(u64 *)p, size = 1ULL << ((id & KVM_REG_SIZE_MASK) >> KVM_REG_SIZE_SHIFT);
		struct kvm_one_reg reg = { .id = id, .addr = (u64)(unsigned long)(p + sizeof(u64)) };
		if (size > (u64)(end - p) - sizeof(u64))
			return -1;		/* the value runs past the record */
		/* what the sender never sends (migrate__send skips them) is not taken either: the
		 * machine's configuration and extensions are this host's */
		if (reg_is_config(id)) {
			p += sizeof(u64) + size;
			continue;
		}
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0) {
			if ((*nfail)++ < 8)
				fprintf(stderr, "migrate: vcpu %u: register 0x%llx (%llu bytes) refused: %s\n", cpu,
					(unsigned long long)id, (unsigned long long)size, strerror(errno));
		} else
			(*nset)++;
		p += sizeof(u64) + size;
	}
	return 0;
}

/* ---- the in-kernel AIA: APLIC registers by offset, IMSIC registers by vcpu and iselect ---- */

static int aplic_regs(u32 *off, int max, int nsrc)
{
	int n = 0, words = (nsrc + 1 + 31) / 32;

	off[n++] = 0x0;						/* domaincfg (written last on restore) */
	for (int i = 1; i <= nsrc && n < max; i++)
		off[n++] = 0x4 + 4 * (i - 1);			/* sourcecfg[i] */
	for (int i = 1; i <= nsrc && n < max; i++)
		off[n++] = 0x3004 + 4 * (i - 1);		/* target[i] */
	for (int k = 0; k < words && n < max; k++)
		off[n++] = 0x1e00 + 4 * k;			/* setie[k] */
	for (int k = 0; k < words && n < max; k++)
		off[n++] = 0x1c00 + 4 * k;			/* setip[k] */
	return n;
}

static int imsic_regs(u32 *sel, int max, int nids)
{
	int n = 0, words = (nids + 1 + 63) / 64;

	sel[n++] = 0x70;					/* eidelivery (last on restore) */
	sel[n++] = 0x72;					/* eithreshold */
	for (int k = 0; k < words && n < max; k++)
		sel[n++] = 0x80 + 2 * k;			/* eip (even only on RV64) */
	for (int k = 0; k < words && n < max; k++)
		sel[n++] = 0xc0 + 2 * k;			/* eie */
	return n;
}

static int aia_attr(int afd, u32 group, u64 attr, u64 *val, bool set)
{
	struct kvm_device_attr a = { .group = group, .attr = attr, .addr = (u64)(unsigned long)val };

	return ioctl(afd, set ? KVM_SET_DEVICE_ATTR : KVM_GET_DEVICE_ATTR, &a);
}

static int send_aia(int fd, struct kvm *kvm)
{
	int afd = aia__fd(), n;
	u32 off[600];
	u64 vals[600], rec[2 * 600];

	if (afd < 0)
		return 0;
	n = aplic_regs(off, 600, aia__nr_sources());
	for (int i = 0; i < n; i++) {
		u32 v = 0;
		struct kvm_device_attr a = { .group = KVM_DEV_RISCV_AIA_GRP_APLIC, .attr = off[i],
					     .addr = (u64)(unsigned long)&v };

		ioctl(afd, KVM_GET_DEVICE_ATTR, &a);
		rec[2 * i] = off[i];
		rec[2 * i + 1] = v;
	}
	if (put(fd, T_APLIC, rec, n * 2 * sizeof(u64)))
		return -1;
	for (int c = 0; c < kvm->nrcpus; c++) {
		u32 sel[64];
		int m = imsic_regs(sel, 64, aia__nr_ids());

		rec[0] = c;
		for (int i = 0; i < m; i++) {
			u64 v = 0;

			aia_attr(afd, KVM_DEV_RISCV_AIA_GRP_IMSIC, KVM_DEV_RISCV_AIA_IMSIC_MKATTR(c, sel[i]), &v, false);
			rec[1 + 2 * i] = sel[i];
			rec[2 + 2 * i] = v;
		}
		if (put(fd, T_IMSIC, rec, (1 + 2 * m) * sizeof(u64)))
			return -1;
	}
	(void)vals;
	return 0;
}

static void apply_aplic(u64 *rec, int n)
{
	int afd = aia__fd();

	/* everything but domaincfg first, then domaincfg (its enable) */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n; i++) {
			u32 v = rec[2 * i + 1];
			struct kvm_device_attr a = { .group = KVM_DEV_RISCV_AIA_GRP_APLIC, .attr = rec[2 * i],
						     .addr = (u64)(unsigned long)&v };

			if ((rec[2 * i] == 0) != (pass == 1))
				continue;
			ioctl(afd, KVM_SET_DEVICE_ATTR, &a);
		}
}

static void apply_imsic(struct kvm *kvm, u64 *rec, int n)
{
	int afd = aia__fd(), cpu;

	if (n < 0 || rec[0] >= (u64)kvm->nrcpus)
		return;
	cpu = rec[0];

	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n; i++) {
			u64 sel = rec[1 + 2 * i], v = rec[2 + 2 * i];

			if ((sel == 0x70) != (pass == 1))	/* delivery last */
				continue;
			aia_attr(afd, KVM_DEV_RISCV_AIA_GRP_IMSIC, KVM_DEV_RISCV_AIA_IMSIC_MKATTR(cpu, sel), &v, true);
		}
}

/* ---- the whole machine, out ---- */

static int migrate_send(struct kvm *kvm, const char *to, char *reply, int len)
{
	char host[64] = "";
	int port = 0, fd, nregs = 0, total_regs = 0;
	struct sockaddr_in sa = { .sin_family = AF_INET };
	struct mig_hdr h = { .magic = MIG_MAGIC };
	u64 t0, t_paused, t_sent, bytes = 0;
	struct disk_image *disk = kvm->nr_disks ? kvm->disks[0] : NULL;
	void *dbuf;
	int one = 1;

	corrupt_load();
	if (sscanf(to, "%63[^:]:%d", host, &port) != 2 || inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		snprintf(reply, len, "migrate: --to ADDR:PORT");
		return -1;
	}
	sa.sin_port = htons(port);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		snprintf(reply, len, "migrate: no destination at %s: %s", to, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	t0 = now_ns();
	kvm__pause(kvm);
	usleep(100 * 1000);	/* the disk's thread finishes what it had taken */
	t_paused = now_ns();
	for (int i = 0; i < kvm->nrcpus; i++) {
		struct kvm_mp_state mp = { 0 };
		u64 v[3] = { 0 };
		u64 ids[3] = { KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_CORE | KVM_REG_RISCV_CORE_REG(regs.pc),
			       KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_TIMER | KVM_REG_RISCV_TIMER_REG(time),
			       KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_TIMER | KVM_REG_RISCV_TIMER_REG(compare) };

		ioctl(kvm->cpus[i]->vcpu_fd, KVM_GET_MP_STATE, &mp);
		for (int k = 0; k < 3; k++) {
			struct kvm_one_reg r = { .id = ids[k], .addr = (u64)(unsigned long)&v[k] };

			ioctl(kvm->cpus[i]->vcpu_fd, KVM_GET_ONE_REG, &r);
		}
		fprintf(stderr, "migrate: vcpu %d leaving: mp %u pc 0x%llx time %llu compare %llu\n", i, mp.mp_state,
			(unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2]);
	}

	h.nrcpus = kvm->nrcpus;
	h.ram_size = kvm->ram_size;
	h.disk_size = disk ? disk->size : 0;
	h.nr_virtio = virtio_mmio_count();
	h.t_source_ns = t_paused;
	if (put(fd, T_HDR, &h, sizeof(h)) || put(fd, T_RAM, kvm->ram_start, kvm->ram_size))
		goto fail;
	bytes += kvm->ram_size;
	if (disk && disk->size) {
		dbuf = malloc(disk->size);
		if (!dbuf || pread(disk->fd, dbuf, disk->size, 0) != (ssize_t)disk->size || put(fd, T_DISK, dbuf, disk->size)) {
			free(dbuf);
			goto fail;
		}
		free(dbuf);
		bytes += disk->size;
	}
	for (int i = 0; i < kvm->nrcpus; i++) {
		if (send_vcpu(fd, kvm->cpus[i], &nregs))
			goto fail;
		total_regs += nregs;
	}
	if (send_aia(fd, kvm) || plic__state_send(fd, put) || virtio_mmio_state_send(kvm, fd, put) ||
	    serial8250__state_send(fd, put) || revise__state_send(fd, put) || put(fd, T_END, NULL, 0))
		goto fail;
	/*
	 * The handoff commits (xv6H's defect 6): the destination says it has the whole machine
	 * and has applied it (READY), and only then does this side say COMMIT and stop. A
	 * destination that refuses a record ends the connection before READY, and the machine
	 * runs on here; one that has said READY runs the machine only on COMMIT.
	 */
	{
		struct timeval tv = { .tv_sec = 60 };
		char c = 0;

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (read(fd, &c, 1) != 1 || c != MIG_READY) {
			fprintf(stderr, "migrate: the destination did not take the machine\n");
			goto fail;
		}
		c = MIG_COMMIT;
		if (write(fd, &c, 1) != 1)
			goto fail;
	}
	t_sent = now_ns();
	close(fd);
	snprintf(reply, len, "migrate: sent to %s: %llu MB, %d vcpu registers; paused for %llu ms (%llu ms to stop)",
		 to, (unsigned long long)(bytes >> 20), total_regs,
		 (unsigned long long)((t_sent - t0) / 1000000), (unsigned long long)((t_paused - t0) / 1000000));
	fprintf(stderr, "%s\n", reply);
	return 0;
fail:
	close(fd);
	kvm__continue(kvm);
	snprintf(reply, len, "migrate: the transfer to %s failed: the machine runs on here", to);
	return -1;
}

static void migrate_ipc(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	char to[128] = "", reply[512] = "";
	int r;

	memcpy(to, msg, len < sizeof(to) - 1 ? len : sizeof(to) - 1);
	r = migrate_send(kvm, to, reply, sizeof(reply));
	if (write(fd, reply, strlen(reply) + 1) < 0)
		pr_warning("migrate: no reply to the asker");
	if (r == 0) {
		/* the machine lives on there: this one stops, paused as it is */
		fprintf(stderr, "migrate: the machine left; this kvmtool exits\n");
		_exit(0);
	}
}

/* ---- the whole machine, in (before the vcpus first run) ---- */

static const char *arrive_facts;

static void *arrive_thread(void *arg)
{
	struct kvm *kvm = arg;
	char buf[512], *save = NULL, *f;

	usleep(300 * 1000);	/* the vcpus are running again */
	snprintf(buf, sizeof(buf), "%s", arrive_facts);
	for (f = strtok_r(buf, ";", &save); f; f = strtok_r(NULL, ";", &save)) {
		char reply[256] = "";

		while (*f == ' ')
			f++;
		if (!*f)
			continue;
		/* one revision at a time: the next waits for the last to be acknowledged */
		for (int tries = 0; tries < 100; tries++) {
			if (revise__apply(kvm, f, reply, sizeof(reply)) == 0 || !strstr(reply, "still owed"))
				break;
			usleep(100 * 1000);
		}
		fprintf(stderr, "migrate: on arrival, %s: %s\n", f, reply);
	}
	return NULL;
}

int migrate__incoming(struct kvm *kvm, int port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
	int lfd, fd, one = 1, nset = 0, nfail = 0, nvcpu = 0;
	struct mig_hdr h;
	struct disk_image *disk = kvm->nr_disks ? kvm->disks[0] : NULL;
	u32 tag;
	u64 len, t0;
	pthread_t th;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 1) < 0)
		die("migrate: cannot listen on port %d: %s", port, strerror(errno));
	fprintf(stderr, "migrate: waiting for a machine on port %d\n", port);
	fd = accept(lfd, NULL, NULL);
	close(lfd);
	if (fd < 0)
		die("migrate: accept: %s", strerror(errno));
	t0 = now_ns();
	for (;;) {
		u8 *buf;

		if (get_hdr(fd, &tag, &len))
			die("migrate: the stream ended early");
		if (tag == T_END)
			break;
		if (tag == T_RAM) {
			if (len != kvm->ram_size || xfer_all(fd, kvm->ram_start, len, false))
				die("migrate: RAM of %llu bytes for a machine of %llu", (unsigned long long)len,
				    (unsigned long long)kvm->ram_size);
			continue;
		}
		/* no record but RAM (above) and the disk is larger than a few MB; nothing is allocated
		 * on the stream's word beyond the disk this machine has */
		if (len > (tag == T_DISK && disk ? (u64)disk->size : 16ULL << 20))
			die("migrate: a record (%u) of %llu bytes", tag, (unsigned long long)len);
		buf = malloc(len ? len : 1);
		if (!buf || (len && xfer_all(fd, buf, len, false)))
			die("migrate: a record of %llu bytes", (unsigned long long)len);
		switch (tag) {
		case T_HDR:
			if (len != sizeof(h))
				die("migrate: a header of %llu bytes", (unsigned long long)len);
			memcpy(&h, buf, sizeof(h));
			if (h.magic != MIG_MAGIC || h.nrcpus != (u32)kvm->nrcpus || h.ram_size != kvm->ram_size)
				die("migrate: the machine does not fit here (%u cpus, %llu MB; this one %d, %llu MB)",
				    h.nrcpus, (unsigned long long)(h.ram_size >> 20), kvm->nrcpus,
				    (unsigned long long)(kvm->ram_size >> 20));
			break;
		case T_DISK:
			if (!disk || len != disk->size || pwrite(disk->fd, buf, len, 0) != (ssize_t)len)
				die("migrate: the disk does not fit here");
			break;
		case T_VCPU:
			if (apply_vcpu(kvm, buf, len, &nset, &nfail) < 0)
				die("migrate: a vcpu record that does not fit (%llu bytes)", (unsigned long long)len);
			nvcpu++;
			break;
		case T_APLIC:
			apply_aplic((u64 *)buf, len / (2 * sizeof(u64)));
			break;
		case T_IMSIC:
			apply_imsic(kvm, (u64 *)buf, len < sizeof(u64) ? -1 : (int)((len / sizeof(u64) - 1) / 2));
			break;
		case T_PLIC:
			plic__state_apply(buf, len);
			break;
		case T_VIRTIO:
			virtio_mmio_state_apply(kvm, buf, len);
			break;
		case T_SERIAL:
			serial8250__state_apply(buf, len);
			break;
		case T_REVISE:
			revise__state_apply(kvm, buf, len);
			break;
		default:
			pr_warning("migrate: record %u of %llu bytes not known here", tag, (unsigned long long)len);
		}
		free(buf);
	}
	/* the whole machine is here and applied: READY, and run it only on the source's COMMIT */
	{
		struct timeval tv = { .tv_sec = 60 };
		char c = MIG_READY;

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (write(fd, &c, 1) != 1 || read(fd, &c, 1) != 1 || c != MIG_COMMIT)
			die("migrate: the source did not commit the handoff: this machine does not run here");
	}
	close(fd);
	virtio_mmio_kick_all(kvm);
	for (int i = 0; i < kvm->nrcpus; i++) {
		struct kvm_mp_state mp = { 0 };
		u64 v[4] = { 0 };
		u64 ids[4] = { KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_CORE | KVM_REG_RISCV_CORE_REG(regs.pc),
			       KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_TIMER | KVM_REG_RISCV_TIMER_REG(time),
			       KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_TIMER | KVM_REG_RISCV_TIMER_REG(compare),
			       KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_CSR | KVM_REG_RISCV_CSR_REG(sip) };

		ioctl(kvm->cpus[i]->vcpu_fd, KVM_GET_MP_STATE, &mp);
		for (int k = 0; k < 4; k++) {
			struct kvm_one_reg r = { .id = ids[k], .addr = (u64)(unsigned long)&v[k] };

			ioctl(kvm->cpus[i]->vcpu_fd, KVM_GET_ONE_REG, &r);
		}
		fprintf(stderr, "migrate: vcpu %d here: mp %u pc 0x%llx time %llu compare %llu sip 0x%llx\n", i,
			mp.mp_state, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
			(unsigned long long)v[3]);
	}
	fprintf(stderr, "migrate: arrived: %d vcpus, %d registers set (%d refused), in %llu ms\n",
		nvcpu, nset, nfail, (unsigned long long)((now_ns() - t0) / 1000000));
	arrive_facts = getenv("XV6H_ARRIVE");
	if (arrive_facts && *arrive_facts)
		pthread_create(&th, NULL, arrive_thread, kvm);
	return 0;
}

static int migrate__init(struct kvm *kvm)
{
	kvm_ipc__register_handler(KVM_IPC_MIGRATE, migrate_ipc);
	return 0;
}
dev_base_init(migrate__init);
