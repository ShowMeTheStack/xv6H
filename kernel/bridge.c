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
#include "cgw.h"

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

// is this machine a port on the switch's own segment? A machine behind
// a gateway is not: its frames reach its gateway and its neighbours
// behind the same gateway, and the root segment never sees them.
static int
on_root(struct vm *vm)
{
  return vm->state == VM_RUNNING && vm->vnet.present && vm->gw == 0;
}

// the VM on the root segment whose virtual NIC owns this MAC, or 0.
static struct vm *
vm_by_mac(const uint8 *mac)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm && vm->state == VM_RUNNING && vm->gw == 0 &&
        memcmp(vm->vnet.mac, mac, ETHADDR_LEN) == 0)
      return vm;
  }
  return 0;
}

// and the one behind this gateway with that MAC.
static struct vm *
vm_by_mac_behind(struct cgw *g, const uint8 *mac)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm && vm->state == VM_RUNNING && vm->gw == g &&
        memcmp(vm->vnet.mac, mac, ETHADDR_LEN) == 0)
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

// tell the conversation table about a frame crossing the uplink. The
// verdict is not obeyed here: what may come in from the uplink is
// already decided by the network address translation in front of it, so
// on this port the table is a record rather than a rule. It is obeyed
// where it decides something -- on a gateway, for a published port.
static void
note(struct mbuf *m, int dir, int who)
{
  struct eth *e = (struct eth *)m->head;

  if (m->len < sizeof(*e) + sizeof(struct ip) || ntohs(e->type) != ETHTYPE_IP)
    return;
  ct_track((uint8 *)m->head + sizeof(*e), m->len - sizeof(*e), dir, who);
}

// may this frame be delivered to this machine? A machine's port is
// closed: what comes in is either part of a conversation the machine
// started -- which the conversation table knows -- or for a port it has
// published, or it is one of the few things a machine cannot work
// without and cannot be reached through: ARP, the answer to its own
// DHCP, the answer to its own name lookup, and ICMP.
//
// Nothing here depends on where the frame came from. The uplink, the
// host's own stack and another guest are all outside this machine, and
// what a machine has not asked for does not arrive from any of them.
static int
inbound_ok(struct vm *vm, struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  uint16 type = ntohs(e->type);

  if (type == ETHTYPE_ARP)
    return 1;
  if (type != ETHTYPE_IP || m->len < sizeof(*e) + sizeof(struct ip))
    return 0;
  uint8 *ip = (uint8 *)m->head + sizeof(*e);
  int iplen = m->len - sizeof(*e);
  struct ip *h = (struct ip *)ip;
  uint hl = (h->vhl & 0xf) * 4;

  if (h->pro == IPPROTO_ICMP)
    return 1;
  if ((h->pro == IPPROTO_UDP || h->pro == IPPROTO_TCP) &&
      iplen >= (int)hl + 4) {
    uint8 *l4 = ip + hl;
    uint16 sport = (l4[0] << 8) | l4[1];
    uint16 dport = (l4[2] << 8) | l4[3];
    if (h->pro == IPPROTO_UDP && (dport == 68 || sport == 67))
      return 1;                       // its lease
    if (h->pro == IPPROTO_UDP && sport == 53)
      return 1;                       // the answer to a name it asked about
    if (ct_track(ip, iplen, CT_IN, vm->id + 1))
      return 1;                       // a reply to something it said
    if (vm_port_lookup(vm, h->pro, dport, 0)) {
      ct_open(ip, iplen);             // and now the rest of it may come
      return 1;
    }
  } else if (ct_track(ip, iplen, CT_IN, vm->id + 1)) {
    return 1;
  }
  vm->net_filtered++;
  if (vm_gripe(&vm->g_filter)) {
    // what it was: an operator reading this needs the frame, not only the count
    uint16 ty = ntohs(e->type);
    if (ty == ETHTYPE_IP && m->len >= (int)(sizeof(*e) + sizeof(struct ip) + 4)) {
      uint8 *ip = (uint8 *)m->head + sizeof(*e);
      struct ip *h = (struct ip *)ip;
      uint8 *l4 = ip + (h->vhl & 0xf) * 4;
      uint32 src = ntohl(h->src);
      printk("hyp: vm %d: frame nobody asked for, not delivered (%ld times): proto %d from %d.%d.%d.%d:%d to port %d\n",
             vm->id, vm->net_filtered, h->pro, (int)(src >> 24), (int)((src >> 16) & 255), (int)((src >> 8) & 255),
             (int)(src & 255), (l4[0] << 8) | l4[1], (l4[2] << 8) | l4[3]);
    } else {
      printk("hyp: vm %d: frame nobody asked for, not delivered (%ld times): ethertype 0x%x\n",
             vm->id, vm->net_filtered, ty);
    }
  }
  mbuffree(m);
  return 0;
}

// the one way a frame reaches a machine.
static void
deliver(struct vm *vm, struct mbuf *m)
{
  if (m->len < sizeof(struct eth)) {
    mbuffree(m);
    return;
  }
  if (inbound_ok(vm, m))
    vvnet_rx(vm, m);
}

// a copy of a broadcast for every gateway's public port, which is where
// an ARP request for a public address is answered. `skip' is a gateway
// whose own broadcast this is.
static void
bcast_to_gateways(struct mbuf *m, struct cgw *skip)
{
  for (int i = 0; i < NCGW; i++) {
    struct cgw *g = cgw_at(i);
    if (g == 0 || g == skip)
      continue;
    struct mbuf *c = clone(m);
    if (c)
      cgw_rx_public(g, c);
  }
}

// a frame the gateway has decided about: it goes to the machine as it
// is. The gateway is the filter for what is behind it, so there is no
// second one here.
void
bridge_to_vm(struct vm *vm, struct mbuf *m)
{
  if (vm == 0 || vm->state != VM_RUNNING || !vm->vnet.present) {
    mbuffree(m);
    return;
  }
  vvnet_rx(vm, m);
}

// a frame from a gateway, leaving by one of its two ports: the switch's
// own segment, or the private one behind it.
void
bridge_from_cgw(struct cgw *g, struct mbuf *m, int pub)
{
  struct eth *e = (struct eth *)m->head;

  if (m->len < sizeof(*e)) {
    mbuffree(m);
    return;
  }
  if (!pub) {
    // inside: to one machine, or to all of them
    if (is_broadcast(e->dhost)) {
      for (int i = 0; i < NVM; i++) {
        struct vm *vm = vmtab[i];
        if (vm && vm->state == VM_RUNNING && vm->gw == g && vm->vnet.present) {
          struct mbuf *c = clone(m);
          if (c)
            bridge_to_vm(vm, c);
        }
      }
      mbuffree(m);
      return;
    }
    bridge_to_vm(vm_by_mac_behind(g, e->dhost), m);
    return;
  }
  // outside: the gateway is one more machine on the switch
  if (is_broadcast(e->dhost)) {
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = vmtab[i];
      if (vm && on_root(vm)) {
        struct mbuf *c = clone(m);
        if (c)
          deliver(vm, c);
      }
    }
    // and the other gateways: one gateway asking for another's public
    // address is how two of them reach each other
    bcast_to_gateways(m, g);
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
    deliver(vm, m);
    return;
  }
  int otherpub = 0;
  struct cgw *other = cgw_by_mac(e->dhost, &otherpub);
  if (other && other != g) {
    // two gateways on the same switch: one reaches the other's public
    // port like anybody else
    if (otherpub)
      cgw_rx_public(other, m);
    else
      mbuffree(m);
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
      struct vm *vm = vmtab[i];
      if (vm && on_root(vm)) {
        struct mbuf *c = clone(m);
        if (c)
          deliver(vm, c);
      }
    }
    bcast_to_gateways(m, 0);
    note(m, CT_IN, 0);
    net_rx(m);
    return;
  }
  struct vm *vm = vm_by_mac(e->dhost);
  if (vm) {
    deliver(vm, m);
    return;
  }
  int pub = 0;
  struct cgw *g = cgw_by_mac(e->dhost, &pub);
  if (g && pub) {
    cgw_rx_public(g, m);
    return;
  }
  note(m, CT_IN, 0);
  net_rx(m); // the host's own MAC, or promiscuous noise
}

// a frame from the host's own stack. takes ownership.
int
bridge_tx_host(struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  if (is_broadcast(e->dhost)) {
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = vmtab[i];
      if (vm && on_root(vm)) {
        struct mbuf *c = clone(m);
        if (c)
          deliver(vm, c);
      }
    }
    bcast_to_gateways(m, 0);
    note(m, CT_OUT, 0);
    return virtio_net_tx(m);
  }
  struct vm *vm = vm_by_mac(e->dhost);
  if (vm) {
    deliver(vm, m);
    return 0;
  }
  int pubside = 0;
  struct cgw *gw = cgw_by_mac(e->dhost, &pubside);
  if (gw && pubside) {
    cgw_rx_public(gw, m);
    return 0;
  }
  if (!uplink_policy(m, 0)) {
    net_policy_dropped++;
    mbuffree(m);
    return -1;
  }
  note(m, CT_OUT, 0);
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
  // what the machine says is what it may be answered about
  note(m, CT_OUT, from->id + 1);
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
  if (from->gw) {
    // its segment is the one behind its gateway: its neighbours, and
    // the gateway itself. The root segment never sees it.
    struct cgw *g = from->gw;
    if (is_broadcast(e->dhost)) {
      for (int i = 0; i < NVM; i++) {
        struct vm *vm = vmtab[i];
        if (vm && vm != from && vm->state == VM_RUNNING && vm->gw == g &&
            vm->vnet.present) {
          struct mbuf *c = clone(m);
          if (c)
            bridge_to_vm(vm, c);
        }
      }
      cgw_rx_internal(g, from, m);
      return;
    }
    if (memcmp(e->dhost, g->mac, ETHADDR_LEN) == 0) {
      cgw_rx_internal(g, from, m);
      return;
    }
    struct vm *to = vm_by_mac_behind(g, e->dhost);
    if (to) {
      bridge_to_vm(to, m);
      return;
    }
    bridge_guest_blocked++;
    mbuffree(m);
    return;
  }
  if (is_broadcast(e->dhost)) {
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = vmtab[i];
      if (vm && vm != from && on_root(vm)) {
        struct mbuf *c = clone(m);
        if (c)
          deliver(vm, c);
      }
    }
    bcast_to_gateways(m, 0);
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
    deliver(vm, m);
    return;
  }
  int gpub = 0;
  struct cgw *g2 = cgw_by_mac(e->dhost, &gpub);
  if (g2 && gpub) {
    cgw_rx_public(g2, m);
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
