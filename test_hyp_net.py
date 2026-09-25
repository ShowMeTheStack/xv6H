#
# networking tests for test-hyp.py: the host's own stack, xv6 and Linux
# guests behind the software switch, TLS, and the egress policy that
# keeps everything away from the laptop and its networks.
#
# they need qemu's user-mode network (NET=1, the default) and an
# internet connection. the Linux tests need linux/Image built with
# networking (linux/build.sh) and the https client in its initramfs
# (linux/build-https.sh).
#
import re, time
import test_hyp_guest as g
import test_hyp_linux as L

# addresses that must never be reachable: the laptop (qemu's gateway),
# a typical home router, a LAN host, a corporate range, link-local.
FORBIDDEN = ["10.0.2.2", "192.168.1.1", "192.168.0.10", "172.16.0.1", "169.254.1.1"]


def run(q, line, timeout=60):
    m = q.mark()
    q.cmd_(line, timeout=timeout)
    return q.output_since(m)


def wait_up(q, timeout=30):
    """wait until the (attached) xv6's ifconfig says the interface is up."""
    end = time.time() + timeout
    while time.time() < end:
        out = run(q, "ifconfig", 10)
        m = re.search(r"ip (\d+\.\d+\.\d+\.\d+) mask", out)
        if m and ("(dhcp)" in out or "(static)" in out):
            return m.group(1)
        time.sleep(1)
    raise AssertionError("interface never came up: " + out)


def ping_counts(out, dst):
    """(transmitted, received) for `ping dst' in out, or None if it printed no
    summary (it was refused by the policy, say).

    Which matters because of what the assertion below is allowed to require.
    A destination inside this system -- the host, a guest, a gateway -- is
    ours, and every packet to it must come back; losing one is a defect. But
    8.8.8.8 is not ours and neither is the wifi in between: this laptop's
    link has run at 44-72 ms with 9 ms of jitter and has lost 16-62% of its
    pings for stretches, and `net-host', `net-tools', `net-conntrack',
    `net-gateway', `net-published' and `ct-net' have all gone red for it.
    What the test is for is that the translation path works, and one reply
    proves that as well as four do. Requiring four proves the internet was
    well, which is not this system's doing either way."""
    i = out.find("PING %s " % dst)
    if i < 0:
        return None
    chunk = out[i:]
    j = chunk.find("PING ", 5)
    if j > 0:
        chunk = chunk[:j]
    m = re.search(r"(\d+) packets transmitted, (\d+) (?:packets )?received", chunk)
    return (int(m.group(1)), int(m.group(2))) if m else None


def xv6_internet(q, https=True):
    """the xv6 stack (host or guest) reaches the internet: ICMP, DNS, HTTP, TLS."""
    out = run(q, "ping 8.8.8.8", 30)
    c = ping_counts(out, "8.8.8.8")
    assert c and c[0] == 4, out          # it sent what it was told to
    assert c[1] >= 1, out                # and the world answered at least once
    out = run(q, "nslookup example.com", 30)
    assert re.search(r"Address: \d+\.\d+\.\d+\.\d+", out), out
    out = run(q, "wget http://example.com", 60)
    assert "HTTP 200" in out and "Example Domain" in out, out
    if https:
        out = run(q, "https -v https://example.com", 120)
        assert "TLS 1.2" in out and "HTTP 200" in out and "Example Domain" in out, out
        assert "verified against embedded roots" in out, out


def xv6_policy(q):
    """the xv6 stack refuses to talk to the laptop or any private network,
    and TLS refuses bad certificates."""
    for ip in FORBIDDEN:
        out = run(q, "ping " + ip, 20)
        assert "blocked by the network policy" in out, (ip, out)
    out = run(q, "https https://expired.badssl.com/", 60)
    assert "certificate expired" in out, out
    out = run(q, "https https://wrong.host.badssl.com/", 60)
    assert "does not match the server name" in out, out
    out = run(q, "https https://self-signed.badssl.com/", 60)
    assert "not trusted" in out, out


def net_tools(q):
    """the tools that read /proc/net say what the stack says, and the ones
    that talk to the network get answers."""
    # /proc/net is where all of them get their state
    out = run(q, "cat /proc/net/route", 10)
    assert "Destination" in out and "eth0" in out, out
    # the file is the routing table itself, not two lines made up from
    # the address: the loopback route is in it, the interface names come
    # from the interfaces, and the default route is the one with a
    # gateway and a zero destination
    assert re.search(r"(?mi)^lo\t0000007F\t00000000\t0001", out), out
    assert re.search(r"(?mi)^eth0\t00000000\t[0-9A-F]{8}\t0003", out), out
    assert re.search(r"(?mi)^eth0\t[0-9A-F]{8}\t00000000\t0001", out), out
    out = run(q, "cat /proc/net/dev", 10)
    assert "eth0" in out and "lo:" in out, out

    # ip, route and arp
    out = run(q, "ip addr show", 15)
    assert re.search(r"inet 10\.0\.2\.\d+/\d+", out), out
    assert "link/ether 52:54:00" in out, out
    out = run(q, "ip link show", 15)
    assert "eth0" in out and "state UP" in out, out
    out = run(q, "ip route show", 15)
    assert re.search(r"default via 10\.0\.2\.2", out), out
    out = run(q, "route -n", 15)
    assert "0.0.0.0" in out and "10.0.2.2" in out and "UG" in out, out
    # the gateway answers ARP, so the cache has it after any traffic
    run(q, "ping 8.8.8.8", 30)
    out = run(q, "arp -n", 15)
    assert "HWaddress" in out, out
    assert re.search(r"10\.0\.2\.\d+\s+ether\s+52:55:0a", out) or \
           re.search(r"10\.0\.2\.\d+\s+ether", out), out
    out = run(q, "ip neigh show", 15)
    assert re.search(r"10\.0\.2\.\d+ dev eth0 lladdr", out), out

    # netstat and ss, over a socket that is definitely there
    run(q, "service httpd start", 30)
    out = run(q, "netstat -ltn", 20)
    assert "LISTEN" in out and ":80" in out, out
    out = run(q, "ss -ltn", 20)
    assert "LISTEN" in out and ":80" in out, out
    out = run(q, "netstat -i", 20)
    assert "eth0" in out and "RX-OK" in out, out
    out = run(q, "netstat -r", 20)
    assert "Destination" in out and "UG" in out, out
    out = run(q, "netstat -s", 20)
    assert "TCP: inuse" in out, out

    # nc against the machine's own web server, which is the loopback
    # path and never leaves the machine
    out = run(q, "printf 'GET /hello.txt HTTP/1.0\\r\\n\\r\\n' | nc 127.0.0.1 80", 30)
    assert "200 OK" in out, out
    out = run(q, "nc -z -v 127.0.0.1 80", 20)
    assert "succeeded" in out, out
    out = run(q, "nc -z 127.0.0.1 81", 20)
    assert "open" not in out, out

    # curl, the same way
    out = run(q, "curl -s http://127.0.0.1/hello.txt", 30)
    assert "hello" in out.lower(), out
    out = run(q, "curl -s -o /tmp/c1 http://127.0.0.1/hello.txt && cat /tmp/c1", 30)
    assert "hello" in out.lower(), out
    out = run(q, "curl -s -I http://127.0.0.1/hello.txt", 30)
    assert "200 OK" in out, out
    out = run(q, "curl -s -w '%{http_code}' -o /dev/null http://127.0.0.1/nope", 30)
    assert "404" in out, out

    # curl over the real internet, and its https path
    out = run(q, "curl -s http://example.com/ | head -5", 60)
    assert "<" in out, out
    out = run(q, "curl -s https://example.com/ | head -5", 90)
    assert "<" in out, out

    # dig and host
    out = run(q, "dig +short example.com", 40)
    assert re.search(r"(?m)^\d+\.\d+\.\d+\.\d+\.?$", out), out
    out = run(q, "dig example.com", 40)
    assert "ANSWER SECTION" in out and "IN\tA" in out, out
    out = run(q, "dig example.com MX", 40)
    assert "ANSWER SECTION" in out or "no " in out, out
    out = run(q, "host example.com", 40)
    assert "has address" in out, out
    out = run(q, "dig +short nx-this-name-does-not-exist-xv6.com", 40)
    assert not re.search(r"(?m)^\d+\.\d+\.\d+\.\d+$", out), out

    # traceroute: behind the NAT the first hop answers, which is enough
    # to show that the time to live and the ICMP errors both work
    out = run(q, "traceroute -m 4 -q 1 -w 2 8.8.8.8", 60)
    assert re.search(r"(?m)^\s*1\s", out), out
    assert re.search(r"\d+\.\d+\.\d+\.\d+", out), out

    # tcpdump: capture a ping and read the file back
    out = run(q, "tcpdump -c 4 -w /tmp/p.pcap & sleep 1; ping 8.8.8.8 2; wait", 60)
    out = run(q, "ls -l /tmp/p.pcap", 15)
    assert re.search(r"\b\d\d+\b", out), out
    out = run(q, "od -A n -t x1 -N 4 /tmp/p.pcap", 15)
    assert "d4 c3 b2 a1" in out, out   # the pcap magic, little endian
    out = run(q, "tcpdump -t -n -r /tmp/p.pcap", 30)
    assert "ICMP echo" in out or "IP " in out or "ARP" in out, out
    out = run(q, "tcpdump -t -n -r /tmp/p.pcap icmp", 30)
    assert "ICMP" in out or out.strip() == "", out


def conntrack(q):
    """the table knows what conversations the machine is having: what it
    started, what came back, and nothing it was not told about."""
    # a freshly booted machine has had one conversation with the world
    # (DHCP), so the file is either empty or holds it; either way it
    # reads, and what it holds has the shape of a conversation
    out = run(q, "cat /proc/net/conntrack", 10)
    assert "sport=" in out or out.strip() == "", out

    # an echo request and its reply: one conversation, with replies
    run(q, "ping 8.8.8.8 2", 30)
    out = run(q, "cat /proc/net/conntrack", 10)
    assert re.search(r"icmp\s+1 \d+ src=10\.0\.2\.\d+ dst=8\.8\.8\.8 type=8 code=0"
                     r" id=\d+ packets=2 replies=2 \[ASSURED\]", out), out

    # a TCP conversation, which goes through its states. The fetch is
    # over the NAT to a public address, so the tuple is this machine's
    # own address to port 80 somewhere else.
    run(q, "curl -s -o /dev/null http://example.com/", 60)
    out = run(q, "cat /proc/net/conntrack", 10)
    m = re.search(r"tcp\s+6 (\d+) (\w+) src=10\.0\.2\.\d+ dst=\d+\.\d+\.\d+\.\d+"
                  r" sport=\d+ dport=80 packets=(\d+) replies=(\d+)", out)
    assert m, out
    # the fetch is over, so the conversation is closing or closed, and
    # both directions carried more than a packet
    assert m.group(2) in ("CLOSE_WAIT", "CLOSE", "ESTABLISHED"), m.group(0)
    assert int(m.group(1)) <= 3600, m.group(0)
    assert int(m.group(3)) >= 2 and int(m.group(4)) >= 2, m.group(0)

    # a name lookup is UDP to port 53, and thirty seconds is all a quiet
    # UDP conversation gets
    run(q, "dig +short example.com", 40)
    out = run(q, "cat /proc/net/conntrack", 10)
    m = re.search(r"udp\s+17 (\d+) src=10\.0\.2\.\d+ dst=10\.0\.2\.3"
                  r" sport=\d+ dport=53", out)
    assert m, out
    assert int(m.group(1)) <= 30, m.group(0)

    # the loopback never crosses the uplink, so nothing that happens on
    # it appears here: this is the table of conversations with the world.
    run(q, "service httpd start", 30)
    run(q, "curl -s -o /dev/null http://127.0.0.1/hello.txt", 30)
    out = run(q, "cat /proc/net/conntrack", 10)
    assert "127.0.0.1" not in out, out

    # a guest's conversations are the host's to see, and they are from
    # the guest's own address, not the host's
    g.start_guest(q, "-n ct1 -c 1 -i dhcp", 90)
    gip = wait_up(q)
    assert gip.startswith("10.0.2.") and gip != "10.0.2.15", gip
    run(q, "ping 8.8.8.8 2", 40)
    g.detach(q)
    out = run(q, "cat /proc/net/conntrack", 10)
    assert re.search(r"icmp\s+1 \d+ src=%s dst=8\.8\.8\.8 type=8" %
                     gip.replace(".", r"\."), out), out
    g.kill_vm(q, "ct1")

    # and it goes when its time is up. An ICMP conversation is kept for
    # ten seconds of quiet and a closed TCP one for sixty, so after a
    # dozen seconds the first is gone and the second is not: what
    # expires is each conversation on its own clock, not the table.
    run(q, "ping 1.1.1.1 1", 30)
    out = run(q, "cat /proc/net/conntrack", 10)
    assert "dst=1.1.1.1" in out, out
    # the timeout is counted in the kernel's ticks, which under an
    # emulator on a busy machine fall behind the wall clock, so the wait
    # is for the thing to happen rather than for a number of seconds
    for _ in range(6):
        run(q, "sleep 4", 20)
        out = run(q, "cat /proc/net/conntrack", 10)
        if "dst=1.1.1.1" not in out:
            break
    assert "dst=1.1.1.1" not in out, out
    assert "dport=80" in out, out          # the closed TCP one is still here


def t_conntrack(QEMU, intc, cpus, log):
    """/proc/net/conntrack, over the uplink, for the host and for a guest."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        wait_up(q)
        conntrack(q)
    finally:
        q.close()


def routes(q):
    """the routing table decides where a packet goes, and the longest
    prefix that matches wins. `ip route get' asks the kernel the same
    question a packet asks it."""
    # what a machine with one interface and a lease starts with: its own
    # subnet on eth0, the default route through the gateway, and the
    # loopback
    out = run(q, "ip route show", 15)
    assert re.search(r"default via 10\.0\.2\.2 dev eth0", out), out
    out = run(q, "ip route get 8.8.8.8", 15)
    assert "8.8.8.8 via 10.0.2.2 dev eth0" in out, out
    out = run(q, "ip route get 10.0.2.99", 15)     # on this subnet: no gateway
    assert "10.0.2.99 dev eth0" in out, out
    out = run(q, "ip route get 127.0.0.1", 15)
    assert "127.0.0.1 dev lo" in out, out

    # two routes, one inside the other. The longer prefix is the answer
    # for what it covers, and the shorter one for everything else.
    run(q, "ip route add 10.9.0.0/16 via 10.0.2.2 dev eth0", 15)
    run(q, "ip route add 10.9.7.0/24 via 10.0.2.3 dev eth0", 15)
    out = run(q, "ip route get 10.9.7.5", 15)
    assert "10.9.7.5 via 10.0.2.3 dev eth0" in out, out
    out = run(q, "ip route get 10.9.8.5", 15)
    assert "10.9.8.5 via 10.0.2.2 dev eth0" in out, out
    out = run(q, "ip route get 10.8.8.8", 15)      # neither: the default
    assert "10.8.8.8 via 10.0.2.2 dev eth0" in out, out

    # both are in the table the tools read
    out = run(q, "ip route show", 15)
    assert "10.9.0.0/16 via 10.0.2.2" in out, out
    assert "10.9.7.0/24 via 10.0.2.3" in out, out
    out = run(q, "route -n", 15)
    assert re.search(r"10\.9\.7\.0\s+10\.0\.2\.3\s+255\.255\.255\.0", out), out

    # take the longer one away and the shorter one answers for it again
    run(q, "ip route del 10.9.7.0/24 via 10.0.2.3", 15)
    out = run(q, "ip route get 10.9.7.5", 15)
    assert "10.9.7.5 via 10.0.2.2 dev eth0" in out, out
    out = run(q, "ip route del 10.9.7.0/24 via 10.0.2.3", 15)
    assert "No such process" in out or "no such" in out.lower(), out

    # among routes of the same length the lower metric wins
    run(q, "ip route add 10.9.6.0/24 via 10.0.2.2 dev eth0 metric 10", 15)
    run(q, "ip route add 10.9.6.0/24 via 10.0.2.3 dev eth0 metric 5", 15)
    out = run(q, "ip route get 10.9.6.1", 15)
    assert "10.9.6.1 via 10.0.2.3 dev eth0" in out, out
    run(q, "ip route del 10.9.6.0/24 via 10.0.2.3", 15)
    out = run(q, "ip route get 10.9.6.1", 15)
    assert "10.9.6.1 via 10.0.2.2 dev eth0" in out, out

    # a next hop this machine cannot reach is not a route
    out = run(q, "ip route add 10.9.5.0/24 via 192.168.77.1 dev eth0", 15)
    assert "unreachable" in out.lower() or "Network" in out, out
    # nor is one through an interface that is not there
    out = run(q, "ip route add 10.9.5.0/24 via 10.0.2.2 dev eth9", 15)
    assert "no such device" in out.lower(), out

    # route(8) spells the same thing differently, and both are here
    run(q, "route add 10.9.4.0 netmask 255.255.255.0 gw 10.0.2.2", 15)
    out = run(q, "ip route get 10.9.4.7", 15)
    assert "10.9.4.7 via 10.0.2.2 dev eth0" in out, out
    run(q, "route del 10.9.4.0 netmask 255.255.255.0 gw 10.0.2.2", 15)

    # changing the table is privileged; asking it is not
    out = run(q, "su user -c 'ip route add 10.9.3.0/24 via 10.0.2.2'", 20)
    assert "not permitted" in out.lower() or "Operation" in out, out
    out = run(q, "su user -c 'ip route get 8.8.8.8'", 20)
    assert "via 10.0.2.2" in out, out

    # and a route is not decoration: traffic follows it. The default
    # route is what reaches the internet, so removing it stops that, and
    # putting it back starts it again.
    run(q, "ip route del default via 10.0.2.2", 15)
    out = run(q, "ping 8.8.8.8 1", 30)
    assert "0 received" in out or "unreachable" in out.lower(), out
    run(q, "ip route add default via 10.0.2.2 dev eth0", 15)
    out = run(q, "ping 8.8.8.8 2", 30)
    assert "2 packets transmitted, 2 received" in out, out


def t_routes(QEMU, intc, cpus, log):
    """the routing table: longest prefix, metrics, and who may change it."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        wait_up(q)
        routes(q)
    finally:
        q.close()


def published(q):
    """a machine's switch port is closed: it reaches out, and what reaches
    in is what it publishes. (The host port and the guest port differ only
    behind a gateway, which is the next thing; here they are the same
    number, because a machine answers on its own address.)

    The difference between a frame that was turned away and one that
    arrived is visible from outside: a connection nobody let in is never
    answered at all, and one that reaches a machine with nothing
    listening is refused at once."""
    g.start_guest(q, "-n p1 -c 1 -i dhcp", 90)
    ip1 = wait_up(q)
    run(q, "nc -l -k 80 > /tmp/got &", 15)
    out = run(q, "ping 8.8.8.8 2", 40)          # out is not the way in
    assert "2 packets transmitted, 2 received" in out, out
    g.detach(q)

    # nothing is published: the connection is not refused, it is not
    # answered, because the SYN never reached the machine
    out = run(q, "vmm port p1", 15)
    assert "publishes nothing" in out, out
    out = run(q, "nc -z -v -w 3 %s 80" % ip1, 30)
    assert "timed out" in out, out
    out = run(q, "vmm port p1", 15)
    assert "turned away" in out, out            # and the host says so

    # publish it, and the same connection is answered
    run(q, "vmm port p1 add 80", 15)
    out = run(q, "vmm port p1", 15)
    assert re.search(r"(?m)^80\s+80\s+tcp", out), out
    out = run(q, "nc -z -v -w 5 %s 80" % ip1, 30)
    assert "succeeded" in out, out

    # and what is sent arrives: the machine has it in a file
    run(q, "echo xv6h-published | nc -w 3 %s 80" % ip1, 30)
    g.attach(q, "p1")
    q.send("\n")
    q.expect(r"\$ ", 30)
    out = run(q, "cat /tmp/got", 15)
    assert "xv6h-published" in out.replace(" ", ""), out
    g.detach(q)

    # a port that is not published stays closed while one that is does
    # not: the filter is by port, not by machine
    out = run(q, "nc -z -v -w 3 %s 7 " % ip1, 30)
    assert "timed out" in out, out

    # stop publishing it and it is closed again -- a new connection has
    # nothing in the conversation table to belong to
    run(q, "vmm port p1 rm 80", 15)
    out = run(q, "nc -z -v -w 3 %s 80" % ip1, 30)
    assert "timed out" in out, out

    # a machine may be started with its ports open, and a published port
    # with nothing behind it is refused rather than ignored
    g.start_guest(q, "-n p2 -c 1 -f /disk1.img -i dhcp -p 80 -p 7000/udp", 90)
    ip2 = wait_up(q)
    g.detach(q)
    out = run(q, "vmm port p2", 15)
    assert re.search(r"(?m)^80\s+80\s+tcp", out), out
    assert re.search(r"(?m)^7000\s+7000\s+udp", out), out
    out = run(q, "nc -z -v -w 5 %s 80" % ip2, 30)
    assert "refused" in out or "succeeded" in out, out

    # one machine reaches the other's published port, and not its closed
    # one: the filter is the machine's, not the host's
    run(q, "vmm port p2 add 81", 15)
    g.attach(q, "p1")
    q.send("\n")
    q.expect(r"\$ ", 30)
    out = run(q, "nc -z -v -w 5 %s 81" % ip2, 30)
    assert "refused" in out or "succeeded" in out, out
    out = run(q, "nc -z -v -w 3 %s 82" % ip2, 30)
    assert "timed out" in out, out
    g.detach(q)

    # only a sound specification is accepted, and only of a machine that
    # is there
    out = run(q, "vmm port p2 add 0", 15)
    assert "not a port" in out, out
    out = run(q, "vmm port p2 rm 99", 15)
    assert "no such" in out.lower(), out
    out = run(q, "vmm port p9 add 80", 15)
    assert "no vm" in out, out

    # and the ports are the machine's own: they go when it does
    g.kill_vm(q, "p1")
    g.kill_vm(q, "p2")


def t_published(QEMU, intc, cpus, log):
    """published ports: a VM accepts what it publishes and nothing else."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        wait_up(q)
        q.assert_no_listeners()
        published(q)
    finally:
        q.close()


def gateway(q):
    """a gateway object: a router inside the host. A machine behind one has
    a private address, reaches the world through the gateway's public one,
    and is reachable only on the ports it publishes. Two gateways may own
    the same private subnet, and two machines behind them may have the
    same address."""
    out = run(q, "vmm gw add gw0 172.17.0.1/16", 20)
    m = re.search(r"gateway gw0: 172\.17\.0\.1/16 inside, (\d+\.\d+\.\d+\.\d+) outside", out)
    assert m, out
    pub0 = m.group(1)
    out = run(q, "vmm gw", 15)
    assert "gw0" in out and pub0 in out and "172.17.0.1/16" in out, out
    out = run(q, "cat /proc/net/gateways", 15)
    assert "gw0" in out and "public %s" % pub0 in out, out

    # a gateway may not own the switch's own subnet, nor take an address
    # on it that another gateway has
    out = run(q, "vmm gw add bad 10.0.2.200/24", 20)
    assert "in use" in out.lower() or "exists" in out.lower(), out
    out = run(q, "vmm gw add bad 172.18.0.1/16 pub %s" % pub0, 20)
    assert "in use" in out.lower() or "exists" in out.lower(), out
    out = run(q, "vmm gw add gw0 172.19.0.1/16", 20)
    assert "exists" in out.lower(), out

    # a machine behind it: its address is the private one, and the
    # gateway is its way out
    g.start_guest(q, "-n c1 -c 1 --nic eth0:172.17.0.2/16,gw=172.17.0.1,dns=10.0.2.3"
                     " -p 80", 90)
    out = run(q, "ifconfig", 30)
    assert "172.17.0.2" in out and "172.17.0.1" in out, out
    run(q, "nc -l -k 80 > /tmp/got &", 15)
    # the world, through the gateway's public address and qemu's NAT
    out = run(q, "ping 8.8.8.8 2", 40)
    assert "2 packets transmitted, 2 received" in out, out
    out = run(q, "nslookup example.com", 40)
    assert re.search(r"Address: \d+\.\d+\.\d+\.\d+", out), out
    # and the gateway itself answers for its own address
    out = run(q, "ping 172.17.0.1 2", 30)
    assert "2 packets transmitted, 2 received" in out, out
    # the host, which sees the public address rather than the private one
    out = run(q, "ping 10.0.2.15 2", 40)
    assert "2 packets transmitted, 2 received" in out, out
    g.detach(q)

    # the host: the gateway answers for its public address
    out = run(q, "ping %s 2" % pub0, 40)
    assert "2 packets transmitted, 2 received" in out, out
    # the private address is not the host's business and does not reach
    out = run(q, "ping 172.17.0.2 1", 30)
    assert "0 received" in out or "blocked" in out or "unreachable" in out.lower(), out
    # a published port reaches the machine behind it, translated
    out = run(q, "nc -z -v -w 5 %s 80" % pub0, 30)
    assert "succeeded" in out, out
    run(q, "echo through-the-gateway | nc -w 3 %s 80" % pub0, 30)
    # an unpublished one does not
    out = run(q, "nc -z -v -w 3 %s 81" % pub0, 30)
    assert "succeeded" not in out, out
    out = run(q, "cat /proc/net/gateways", 15)
    m = re.search(r"snat (\d+) dnat (\d+)", out)
    assert m and int(m.group(1)) > 0 and int(m.group(2)) > 0, out

    g.attach(q, "c1")
    q.send("\n")
    q.expect(r"\$ ", 30)
    out = run(q, "cat /tmp/got", 15)
    assert "through-the-gateway" in out.replace(" ", ""), out
    g.detach(q)

    # a second gateway with the same subnet, and a machine behind it with
    # the same address as the first one's
    out = run(q, "vmm gw add gw1 172.17.0.1/16", 20)
    m = re.search(r"(\d+\.\d+\.\d+\.\d+) outside", out)
    assert m, out
    pub1 = m.group(1)
    assert pub1 != pub0, (pub0, pub1)
    g.start_guest(q, "-n c2 -c 1 -f /disk1.img --nic 172.17.0.2/16,gw=172.17.0.1"
                     " --gw gw1 -p 80", 90)
    out = run(q, "ifconfig", 30)
    assert "172.17.0.2" in out, out
    run(q, "nc -l -k 80 > /tmp/got &", 15)
    g.detach(q)

    # they have the same address and cannot see each other by it; they
    # reach each other through the public ones
    g.attach(q, "c1")
    q.send("\n")
    q.expect(r"\$ ", 30)
    out = run(q, "ping 172.17.0.2 1", 30)     # which is itself
    assert "1 packets transmitted, 1 received" in out, out
    out = run(q, "echo between-gateways | nc -w 5 %s 80" % pub1, 40)
    g.detach(q)
    g.attach(q, "c2")
    q.send("\n")
    q.expect(r"\$ ", 30)
    out = run(q, "cat /tmp/got", 15)
    assert "between-gateways" in out.replace(" ", ""), out
    g.detach(q)

    # a gateway with a machine behind it is not taken away from it
    out = run(q, "vmm gw rm gw1", 20)
    assert "busy" in out.lower() or "in use" in out.lower(), out
    g.kill_vm(q, "c2")
    out = run(q, "vmm gw rm gw1", 20)
    assert "removed" in out, out
    g.kill_vm(q, "c1")
    run(q, "vmm gw rm gw0", 20)
    out = run(q, "vmm gw", 15)
    assert "gw0" not in out, out


def t_gateway(QEMU, intc, cpus, log):
    """gateway objects: translation, published ports, and two subnets that
    are the same subnet."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        wait_up(q)
        q.assert_no_listeners()
        gateway(q)
    finally:
        q.close()


def t_net_tools(QEMU, intc, cpus, log):
    """netstat, ss, ip, route, arp, nc, curl, dig, traceroute and tcpdump."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        wait_up(q)
        q.assert_no_listeners()
        net_tools(q)
    finally:
        q.close()


def t_host_net(QEMU, intc, cpus, log):
    """the host xv6 configures itself by DHCP and reaches the internet."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        ip = wait_up(q)
        assert ip.startswith("10.0.2."), ip
        xv6_internet(q)
        xv6_policy(q)
        out = run(q, "ifconfig")
        m = re.search(r"(\d+) blocked by policy", out)
        assert m and int(m.group(1)) >= len(FORBIDDEN), out
    finally:
        q.close()


def t_guest_net_dhcp(QEMU, intc, cpus, log):
    """an xv6 guest gets its own DHCP lease through the switch and reaches
    the internet, including TLS; its policy blocks the laptop too."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        g.start_guest(q, "-n g1 -c 2 -i dhcp", 90)
        ip = wait_up(q)
        assert ip.startswith("10.0.2.") and ip != "10.0.2.15", ip
        out = run(q, "ifconfig")
        assert "52:54:00:78:76:" in out and "(dhcp)" in out, out
        xv6_internet(q)
        xv6_policy(q)
        g.detach(q)
        out = run(q, "vmm info g1")
        assert "net: dhcp" in out and "ip=dhcp" in out, out
        g.kill_vm(q, "g1")
    finally:
        q.close()


def t_guest_net_static(QEMU, intc, cpus, log):
    """two xv6 guests with static addresses talk to each other and to the
    host through the switch, and reach the internet."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        q.send("vmm start -n s1 -f /disk0.img -i 10.0.2.40/24 -d\n")
        q.expect(r"vm \d+ s1:", 20)
        q.expect(r"\$ ", 10)
        q.send("vmm start -n s2 -f /disk1.img -i 10.0.2.41/24,10.0.2.2,10.0.2.3 -d\n")
        q.expect(r"vm \d+ s2:", 20)
        q.expect(r"\$ ", 10)
        out = run(q, "vmm info s2")
        assert "net: static 10.0.2.41 mask 255.255.255.0 gw 10.0.2.2 dns 10.0.2.3" in out, out
        time.sleep(5)
        g.attach(q, "s1")
        q.expect(r"\$ ", 60)
        ip = wait_up(q)
        assert ip == "10.0.2.40", ip
        out = run(q, "ifconfig")
        assert "(static)" in out, out
        out = run(q, "ping 10.0.2.41", 30)  # guest to guest
        assert "4 packets transmitted, 4 received" in out, out
        out = run(q, "ping 10.0.2.15", 30)  # guest to host
        assert "4 packets transmitted, 4 received" in out, out
        xv6_internet(q, https=False)
        for ip in FORBIDDEN:
            out = run(q, "ping " + ip, 20)
            assert "blocked by the network policy" in out, (ip, out)
        g.detach(q)
        # the host sees both guests.
        out = run(q, "ping 10.0.2.41", 30)
        assert "4 packets transmitted, 4 received" in out, out
        run(q, "vmm kill --all", 60)
    finally:
        q.close()


def linux_internet(q, https=True):
    out = L.linux_cmd(q, "ping -c 2 -W 5 8.8.8.8", 40)
    # as everywhere else in this file: it must send what it was told to, and
    # the world must answer at least once. Requiring both replies is a
    # statement about the wifi (see ping_counts above), and this laptop's has
    # failed this test twice in an afternoon for one lost packet.
    # busybox says "1 packets received" where xv6's own ping says "1
    # received", and a regex that knows only one of the two reads a lost
    # packet as a lost test.
    m = re.search(r"(\d+) packets transmitted, (\d+) (?:packets )?received", out)
    assert m and int(m.group(1)) == 2, out
    assert int(m.group(2)) >= 1, out
    out = L.linux_cmd(q, "nslookup example.com", 40)
    assert re.search(r"Address: *\d+\.\d+\.\d+\.\d+", out), out
    out = L.linux_cmd(q, "wget -q -O - http://example.com", 60)
    assert "Example Domain" in out, out
    if https:
        out = L.linux_cmd(q, "https -v https://example.com", 120)
        assert "TLS 1.2" in out and "HTTP 200" in out and "Example Domain" in out, out
        out = L.linux_cmd(q, "https https://wrong.host.badssl.com/", 60)
        assert "does not match the server name" in out, out


def linux_blocked(q):
    """Linux has no policy of its own: the switch drops what it sends to
    the laptop and private networks."""
    for ip in FORBIDDEN:
        out = L.linux_cmd(q, "ping -c 1 -W 3 " + ip, 30)
        assert "100% packet loss" in out, (ip, out)


def t_linux_net_dhcp(QEMU, intc, cpus, log):
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        L.start_linux(q, "-f none -c 2 -H -i dhcp", timeout=150)
        out = L.linux_cmd(q, "ip -4 addr show eth0")
        m = re.search(r"inet (10\.0\.2\.\d+)/24", out)
        assert m and m.group(1) != "10.0.2.15", out
        out = L.linux_cmd(q, "cat /etc/resolv.conf")
        assert "nameserver 10.0.2.3" in out, out
        linux_internet(q, https=L.have_https(q))
        linux_blocked(q)
        out = L.linux_cmd(q, "ping -c 1 -W 3 10.0.2.15")  # Linux to the host
        assert "1 packets received" in out, out
        L.poweroff(q)
        out = run(q, "ifconfig")
        m = re.search(r"(\d+) blocked by policy", out)
        assert m and int(m.group(1)) >= len(FORBIDDEN), out
    finally:
        q.close()


def t_linux_net_static(QEMU, intc, cpus, log):
    """a static Linux guest and a static xv6 guest see each other."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        q.send("vmm start -n s1 -f /disk0.img -i 10.0.2.40/24 -d\n")
        q.expect(r"vm \d+ s1:", 20)
        q.expect(r"\$ ", 10)
        L.start_linux(q, "-f none -c 1 -i 10.0.2.50/24", timeout=150)
        out = L.linux_cmd(q, "ip -4 addr show eth0")
        assert "inet 10.0.2.50/24" in out, out
        out = L.linux_cmd(q, "cat /proc/net/pnp")
        assert "nameserver 10.0.2.3" in out, out
        out = L.linux_cmd(q, "ping -c 2 -W 5 10.0.2.40", 40)  # Linux to xv6 guest
        assert "2 packets received" in out, out
        linux_internet(q, https=False)
        linux_blocked(q)
        L.poweroff(q)
        g.attach(q, "s1")
        q.expect(r"\$ ", 30)
        g.detach(q)
        run(q, "vmm kill --all", 60)
    finally:
        q.close()


def tests(modes, cpus, log, QEMU, have_linux):
    t = []
    for m in modes:
        t.append(("net-host-%s" % m, lambda m=m: t_host_net(QEMU, m, cpus, log)))
        t.append(("net-guest-dhcp-%s" % m, lambda m=m: t_guest_net_dhcp(QEMU, m, cpus, log)))
    t.append(("net-guest-static", lambda: t_guest_net_static(QEMU, modes[0], cpus, log)))
    t.append(("net-tools", lambda: t_net_tools(QEMU, modes[0], cpus, log)))
    t.append(("net-conntrack", lambda: t_conntrack(QEMU, modes[0], cpus, log)))
    t.append(("net-routes", lambda: t_routes(QEMU, modes[0], cpus, log)))
    t.append(("net-published", lambda: t_published(QEMU, modes[0], cpus, log)))
    t.append(("net-gateway", lambda: t_gateway(QEMU, modes[0], cpus, log)))
    if have_linux:
        for m in modes:
            t.append(("net-linux-dhcp-%s" % m, lambda m=m: t_linux_net_dhcp(QEMU, m, cpus, log)))
        t.append(("net-linux-static", lambda: t_linux_net_static(QEMU, modes[-1], cpus, log)))
    return t
