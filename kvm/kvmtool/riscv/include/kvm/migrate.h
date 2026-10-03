/* kvmtool migration on RISC-V (riscv/migrate.c) and each device's part of it */
#ifndef KVM__MIGRATE_H
#define KVM__MIGRATE_H

#include <linux/types.h>

struct kvm;

enum {
	T_HDR = 1, T_RAM, T_DISK, T_VCPU, T_APLIC, T_IMSIC, T_PLIC, T_VIRTIO, T_SERIAL, T_REVISE, T_END
};
typedef int (*mig_put_t)(int fd, u32 tag, const void *data, u64 len);

int migrate__incoming(struct kvm *kvm, int port);

/* riscv/aia.c */
int aia__fd(void);
int aia__nr_sources(void);
int aia__nr_ids(void);
/* riscv/plic.c */
int plic__state_send(int fd, mig_put_t put);
void plic__state_apply(void *buf, u64 len);
/* virtio/mmio.c */
int virtio_mmio_state_send(struct kvm *kvm, int fd, mig_put_t put);
void virtio_mmio_state_apply(struct kvm *kvm, void *buf, u64 len);
void virtio_mmio_kick_all(struct kvm *kvm);
/* hw/serial.c */
int serial8250__state_send(int fd, mig_put_t put);
void serial8250__state_apply(void *buf, u64 len);
/* riscv/revise.c, riscv/revise-facts.c */
int revise__state_send(int fd, mig_put_t put);
void revise__state_apply(struct kvm *kvm, void *buf, u64 len);
int revise_facts__state_get(void *buf, int max);
void revise_facts__state_put(struct kvm *kvm, const void *buf, int len);

#endif
