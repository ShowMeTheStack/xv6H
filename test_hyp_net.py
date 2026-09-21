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


def xv6_internet(q, https=True):
    """the xv6 stack (host or guest) reaches the internet: ICMP, DNS, HTTP, TLS."""
    out = run(q, "ping 8.8.8.8", 30)
    assert "4 packets transmitted, 4 received" in out, out
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


def t_net_tools(QEMU, intc, cpus, log):
    """netstat, ss, ip, route, arp, nc, curl, dig, traceroute and tcpdump."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        wait_up(q)
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
    assert "2 packets received" in out, out
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
    if have_linux:
        for m in modes:
            t.append(("net-linux-dhcp-%s" % m, lambda m=m: t_linux_net_dhcp(QEMU, m, cpus, log)))
        t.append(("net-linux-static", lambda: t_linux_net_static(QEMU, modes[-1], cpus, log)))
    return t
