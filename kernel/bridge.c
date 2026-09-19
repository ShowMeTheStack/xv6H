//
// the software switch: the physical NIC (uplink), the host's own stack,
// and one virtual NIC per running VM are its ports. guest MACs are
// assigned by the hypervisor, so forwarding is by lookup, not learning.
//
// what leaves through the uplink is subject to net_policy_ok(): guests
// (and the host) reach DHCP, the DNS forwarder and the public internet,
// nothing on the laptop or any private network. traffic between guests
// and the host stays inside the switch.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "net.h"
#include "hyp.h"

static struct spinlock bridgelock;
uint64 bridge_guest_blocked;

void
bridgeinit(void)
{
  initlock(&bridgelock, "bridge");
}

static int
is_broadcast(const uint8 *mac)
{
  return mac[0] & 1; // group bit: broadcast and multicast
}

// the VM whose virtual NIC owns this MAC, or 0.
static struct vm *
vm_by_mac(const uint8 *mac)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = &vms[i];
    if (vm->state == VM_RUNNING && memcmp(vm->vnet.mac, mac, ETHADDR_LEN) == 0)
      return vm;
  }
  return 0;
}

static struct mbuf *
clone(struct mbuf *m)
{
  struct mbuf *c = mbufalloc(m->head - m->buf);
  if (c == 0)
    return 0;
  memmove(mbufput(c, m->len), m->head, m->len);
  return c;
}

// may an ethernet frame from a guest leave through the uplink?
static int
uplink_policy(struct mbuf *m, int from_guest)
{
  struct eth *e = (struct eth *)m->head;
  uint16 type = ntohs(e->type);
  if (type == ETHTYPE_ARP)
    return 1;
  if (type != ETHTYPE_IP || m->len < sizeof(*e) + sizeof(struct ip))
    return 0;
  struct ip *h = (struct ip *)(m->head + sizeof(*e));
  uint32 dst = ntohl(h->dst);
  uint16 dport = 0;
  uint hl = (h->vhl & 0xf) * 4;
  if ((h->pro == IPPROTO_UDP || h->pro == IPPROTO_TCP) && m->len >= sizeof(*e) + hl + 4) {
    uint8 *p = (uint8 *)h + hl;
    dport = (p[2] << 8) | p[3];
  }
  return net_policy_ok(dst, h->pro, dport, from_guest);
}

// a frame arrived on the uplink. takes ownership.
void
bridge_rx_uplink(struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  if (m->len < sizeof(*e)) {
    mbuffree(m);
    return;
  }
  if (is_broadcast(e->dhost)) {
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = &vms[i];
      if (vm->state == VM_RUNNING && vm->vnet.present) {
        struct mbuf *c = clone(m);
        if (c)
          vvnet_rx(vm, c);
      }
    }
    net_rx(m);
    return;
  }
  struct vm *vm = vm_by_mac(e->dhost);
  if (vm) {
    vvnet_rx(vm, m);
    return;
  }
  net_rx(m); // the host's own MAC, or promiscuous noise
}

// a frame from the host's own stack. takes ownership.
int
bridge_tx_host(struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  if (is_broadcast(e->dhost)) {
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = &vms[i];
      if (vm->state == VM_RUNNING && vm->vnet.present) {
        struct mbuf *c = clone(m);
        if (c)
          vvnet_rx(vm, c);
      }
    }
    return virtio_net_tx(m);
  }
  struct vm *vm = vm_by_mac(e->dhost);
  if (vm) {
    vvnet_rx(vm, m);
    return 0;
  }
  if (!uplink_policy(m, 0)) {
    net_policy_dropped++;
    mbuffree(m);
    return -1;
  }
  return virtio_net_tx(m);
}

// a frame transmitted by a guest. takes ownership.
void
bridge_tx_guest(struct vm *from, struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  if (m->len < sizeof(*e) || memcmp(e->shost, from->vnet.mac, ETHADDR_LEN) != 0) {
    // guests may not forge source addresses
    bridge_guest_blocked++;
    mbuffree(m);
    return;
  }
  if (ntohs(e->type) == ETHTYPE_ARP && m->len >= sizeof(*e) + sizeof(struct arp)) {
    // nor claim another port's MAC, or the gateway's / dns's address:
    // that would let one guest capture the host's or its neighbours'
    // internet traffic.
    struct arp *a = (struct arp *)(m->head + sizeof(*e));
    uint32 spa = ntohl(a->sip);
    if (memcmp(a->sha, from->vnet.mac, ETHADDR_LEN) != 0 || spa == netcfg.gw || spa == netcfg.dns ||
        spa == netcfg.ip) {
      bridge_guest_blocked++;
      mbuffree(m);
      return;
    }
  }
  if (is_broadcast(e->dhost)) {
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = &vms[i];
      if (vm != from && vm->state == VM_RUNNING && vm->vnet.present) {
        struct mbuf *c = clone(m);
        if (c)
          vvnet_rx(vm, c);
      }
    }
    struct mbuf *h = clone(m);
    if (h)
      net_rx(h);
    if (uplink_policy(m, 1)) {
      virtio_net_tx(m);
    } else {
      bridge_guest_blocked++;
      mbuffree(m);
    }
    return;
  }
  if (memcmp(e->dhost, netcfg.mac, ETHADDR_LEN) == 0) {
    net_rx(m);
    return;
  }
  struct vm *vm = vm_by_mac(e->dhost);
  if (vm) {
    vvnet_rx(vm, m);
    return;
  }
  if (!uplink_policy(m, 1)) {
    bridge_guest_blocked++;
    net_policy_dropped++;
    mbuffree(m);
    return;
  }
  virtio_net_tx(m);
}
