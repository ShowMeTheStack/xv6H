/* Revisable machine descriptions in kvmtool: the protocol state (riscv/revise.c) and the
 * fact table (riscv/revise-facts.c). */
#ifndef KVM__RISCV_REVISE_H
#define KVM__RISCV_REVISE_H

#include <linux/types.h>
#include <pthread.h>
#include <stdbool.h>

struct kvm;
struct kvm_cpu;
enum irq_type;

#define REVISE_ANCHOR_BASE	0x1f000000ULL	/* never moves: outside every revisable window */
#define REVISE_ANCHOR_SIZE	0x1000
#define REVISE_DTB_MAX		0x10000		/* one description slot (= FDT_MAX_SIZE) */

/* what a revision changed (the module's REV_* bits, linux/machrev/machrev.c) */
/* the bits are xv6H's (kernel/hyp.h); the first port had INTC and DEVICES swapped */
#define REV_DEVICES	0x01
#define REV_INTC	0x02
#define REV_VLEN	0x04
#define REV_SBI		0x08
#define REV_TIMER	0x10
#define REV_MEM		0x20
#define REV_MEMORY	0x40

struct revise_state {
	int enabled, capable;
	pthread_mutex_t lock;
	pthread_cond_t acked_cv;
	u32 gen, acked, described, what, adopted, stale;
	u64 base_gpa, slot_gpa[2], desc_gpa;
	int next_slot;
	int irq, line;
	u64 t_ask, t_pending, t_describe, t_done;
	void (*ack_hook)(struct kvm *kvm, u32 what, u32 adopted);
	void (*describe_hook)(struct kvm *kvm);
	long (*request_hook)(struct kvm *kvm, unsigned long what, long *val);
};
extern struct revise_state revise;
extern struct kvm *revise_kvm;	/* the machine, for the state hooks (A.6) */

/* what the description says of each cpu, recorded from KVM at boot and edited by facts */
struct revise_cpu_desc {
	char isa[1024];
	unsigned long cbom, cboz, cbop, satp, xlen;
	u32 hartid;		/* F9: the number the description gives this cpu */
	int online;		/* F7/F8 */
};
#ifndef KVM_NR_CPUS
#define KVM_NR_CPUS 512
#endif
extern struct revise_cpu_desc revise_cpu[KVM_NR_CPUS];
extern int revise_cpu_cached;
void revise__record_cpu(int cpu, const char *isa, unsigned long cbom, unsigned long cboz,
			unsigned long cbop, unsigned long satp, unsigned long xlen);
void revise__generate_cpu_node(void *fdt, struct kvm *kvm, int cpu);

bool revise__sbi(struct kvm_cpu *vcpu);
int revise__ask(struct kvm *kvm, u32 what);
void revise__generate_fdt_nodes(void *fdt, struct kvm *kvm, void (*irq_prop)(void *, u8, enum irq_type));
void revise__generate_reserved_memory(void *fdt, struct kvm *kvm);
int revise__apply(struct kvm *kvm, const char *req, char *reply, int len);
int revise_facts__init(struct kvm *kvm);
int revise_facts__rate_now(struct kvm_cpu *vcpu);	/* F6: MACH call 7 */
int riscv__fdt_build(struct kvm *kvm, u64 gpa);

#endif
