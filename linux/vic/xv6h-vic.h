/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xv6H: the description-bound root interrupt controller (drivers/irqchip/irq-xv6h-vic.c),
 * as the machrev driver drives it when the description changes.
 */
#ifndef _LINUX_IRQCHIP_XV6H_VIC_H
#define _LINUX_IRQCHIP_XV6H_VIC_H

#include <linux/types.h>

#define XV6H_VIC_PLIC	0
#define XV6H_VIC_APLIC	1
#define XV6H_VIC_IMSIC	2

#ifdef CONFIG_XV6H_VIC
int xv6h_vic_rebind(int kind, phys_addr_t base, resource_size_t size);	/* F1, F2: another controller, or another address */
int xv6h_vic_rehart(void);						/* F9: contexts, IDCs and targets follow the hart numbers */
int xv6h_vic_kind(void);
#else
static inline int xv6h_vic_rebind(int kind, phys_addr_t base, resource_size_t size) { return -ENODEV; }
static inline int xv6h_vic_rehart(void) { return -ENODEV; }
static inline int xv6h_vic_kind(void) { return -1; }
#endif

#endif
