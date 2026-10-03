#!/usr/bin/env python3
# Map pc samples (tools/zedboard.py restore's "B4 PCS pc/cause/vsepc ...") to the guest's symbols.
#   python3 tools/pcprof.py results.out ~/xv6h-linux/obj-hf/System.map
import bisect, collections, re, sys
line = next(l for l in open(sys.argv[1], errors="replace") if l.startswith("B4 PCS"))
syms = []
for l in open(sys.argv[2]):
    a, t, n = l.split()[:3]
    if t.lower() in "tw":
        syms.append((int(a, 16), n))
syms.sort()
addrs = [a for a, _ in syms]
def name(x):
    i = bisect.bisect_right(addrs, x) - 1
    return "%s+0x%x" % (syms[i][1], x - syms[i][0]) if i >= 0 else hex(x)
c = collections.Counter(); causes = collections.Counter()
for s in line.split()[2:]:
    pc, cause, _ = s.split("/")
    c[name(int(pc, 16)).split("+")[0]] += 1
    causes[cause] += 1
n = sum(c.values())
for f, k in c.most_common(25):
    print("%4d %5.1f%%  %s" % (k, 100.0 * k / n, f))
print("last exit causes:", dict(causes))
