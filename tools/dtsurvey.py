#!/usr/bin/env python3
# dtsurvey: which of the paper's facts differ across real RISC-V platforms.
#
# Reads compiled device trees (*.dtb) under the directories given, decompiles each with dtc,
# and extracts, per board: model and compatible, timebase-frequency (F6), interrupt controller
# kinds (F1: PLIC / APLIC / IMSIC / vendor), hart IDs from the cpu nodes' reg (F7/F8 count,
# F9 numbering: non-zero first ID, gaps), cache-block sizes (riscv,cboz/cbom/cbop-block-size,
# F21), ISA extensions (riscv,isa-extensions or riscv,isa; V and Zicboz for F10/F21), the
# UART and virtio/ethernet base addresses (F15 device map), and memory size (F13).
# Every value comes from a dtb the kernel tree builds; nothing is typed in by hand.
#   python3 tools/dtsurvey.py OUT.md DTB_DIR [DTB_DIR...]
import os, re, subprocess, sys, collections

def dts_of(path):
    return subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", path], capture_output=True, text=True).stdout

def nodes(dts):
    """yield (path, body) for every node, with the body excluding child nodes"""
    stack, out, cur = [], [], []
    for line in dts.splitlines():
        s = line.strip()
        m = re.match(r"^(?:[\w-]+:\s*)?([\w@,.+-]+|/)\s*\{$", s)
        if m:
            stack.append((m.group(1), cur)); cur = []
            continue
        if s == "};":
            name, parent = stack.pop()
            path = "/".join(n for n, _ in stack) + "/" + name
            out.append((path.replace("//", "/"), "\n".join(cur)))
            cur = parent
            continue
        cur.append(s)
    return out

def prop(body, name):
    m = re.search(r"(?m)^" + re.escape(name) + r"\s*=\s*(.*);$", body)
    return m.group(1) if m else None

def cells(v):
    return [int(x, 16) if x.startswith("0x") else int(x) for x in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", v or "")]

def strings(v):
    return re.findall(r'"([^"]*)"', v or "")

def survey(path):
    dts = dts_of(path)
    if not dts:
        return None
    ns = nodes(dts)
    root = next((b for p, b in ns if p == "/"), "")
    r = {"file": os.path.relpath(path), "model": (strings(prop(root, "model")) or ["?"])[0]}
    tb = None
    for p, b in ns:
        if p.endswith("/cpus"):
            v = cells(prop(b, "timebase-frequency"))
            if v: tb = v[0]
    harts, isa, cboz, cbom, vlen = [], set(), set(), set(), set()
    for p, b in ns:
        if re.search(r"/cpus/cpu@[0-9a-f]+$", p) and "riscv" in (prop(b, "compatible") or ""):
            st = strings(prop(b, "status"))
            if st and st[0] not in ("okay", "ok"):
                continue
            reg = cells(prop(b, "reg"))
            if reg: harts.append(reg[-1])
            v = cells(prop(b, "timebase-frequency"))
            if v and tb is None: tb = v[0]
            ex = strings(prop(b, "riscv,isa-extensions"))
            if not ex:
                s = (strings(prop(b, "riscv,isa")) or [""])[0]
                base = re.match(r"rv\d+([a-z]*)", s)
                ex = list(base.group(1)) if base else []
                ex += [x for x in s.split("_")[1:] if x]
            isa |= set(e.lower() for e in ex)
            for k, dst in (("riscv,cboz-block-size", cboz), ("riscv,cbom-block-size", cbom)):
                v = cells(prop(b, k))
                if v: dst.add(v[0])
    r["timebase"] = tb
    r["harts"] = sorted(harts)
    r["vector"] = "yes" if ("v" in isa or any(e.startswith("zve") for e in isa)) else ("xtheadvector" if "xtheadvector" in isa else "no")
    r["zicboz"] = "zicboz" in isa
    r["cboz"] = sorted(cboz); r["cbom"] = sorted(cbom)
    intc = set()
    for p, b in ns:
        c = " ".join(strings(prop(b, "compatible")))
        if "interrupt-controller" not in b and "interrupt-controller" not in p:
            continue
        if "riscv,cpu-intc" in c:
            continue
        if "riscv,imsics" in c: intc.add("IMSIC")
        elif "riscv,aplic" in c: intc.add("APLIC")
        elif "plic" in c or "riscv,plic0" in c or "sifive,plic" in c: intc.add("PLIC")
        elif "clint" in c or "aclint" in c: pass
        elif c: intc.add("other:" + c.split()[0])
    r["intc"] = sorted(i for i in intc if not i.startswith("other:")) or ["?"]
    uart = None
    for p, b in ns:
        c = " ".join(strings(prop(b, "compatible")))
        if any(k in c for k in ("ns16550", "uart", "serial")) and "@" in p.split("/")[-1]:
            uart = p.split("@")[-1]; break
    r["uart"] = uart
    mem = 0
    for p, b in ns:
        if (prop(b, "device_type") or "").strip('"') == "memory" or re.search(r"/memory@", p):
            c = cells(prop(b, "reg"))
            if len(c) >= 4: mem += sum((c[i+2] << 32) + c[i+3] for i in range(0, len(c) - 3, 4))
    r["mem_mb"] = mem >> 20 if mem else None
    return r

def main():
    out, dirs = sys.argv[1], sys.argv[2:]
    rows = []
    for d in dirs:
        for dp, _, fs in os.walk(d):
            for f in sorted(fs):
                if f.endswith(".dtb"):
                    r = survey(os.path.join(dp, f))
                    if r: rows.append(r)
    rows.sort(key=lambda r: r["file"])
    qv = next((r for r in rows if "qemu" in r["file"].lower() and "aia-none" in r["file"]), None)
    with open(out, "w") as o:
        o.write("# Real RISC-V device trees: the facts that differ (tools/dtsurvey.py)\n\n")
        o.write("Sources: %s\n\n" % ", ".join(dirs))
        o.write("| board (dtb) | model | timebase Hz | controller | harts (IDs) | V | Zicboz | cboz B | UART | RAM MB |\n|---|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            h = r["harts"]
            o.write("| %s | %s | %s | %s | %d (%s) | %s | %s | %s | %s | %s |\n" % (
                r["file"], r["model"], r["timebase"], "+".join(r["intc"]), len(h),
                ",".join(map(str, h)) if len(h) <= 8 else "%d..%d" % (h[0], h[-1]) if h else "",
                r["vector"], "yes" if r["zicboz"] else "no", ",".join(map(str, r["cboz"])) or "-", r["uart"], r["mem_mb"]))
        n = len(rows)
        tbs = collections.Counter(r["timebase"] for r in rows)
        o.write("\n## Summary over %d device trees\n\n" % n)
        o.write("- timebase-frequency: %d distinct values: %s\n" % (len(tbs), ", ".join("%s Hz (%d)" % (k, v) for k, v in tbs.most_common())))
        ic = collections.Counter("+".join(r["intc"]) for r in rows)
        o.write("- interrupt controller: %s\n" % ", ".join("%s (%d)" % kv for kv in ic.most_common()))
        nz = [r for r in rows if r["harts"] and r["harts"][0] != 0]
        gap = [r for r in rows if r["harts"] and r["harts"] != list(range(r["harts"][0], r["harts"][0] + len(r["harts"])))]
        o.write("- hart numbering: %d of %d start at a non-zero hart ID; %d have gaps\n" % (len(nz), n, len(gap)))
        hc = collections.Counter(len(r["harts"]) for r in rows)
        o.write("- hart count: %s\n" % ", ".join("%d harts (%d)" % kv for kv in sorted(hc.items())))
        vc = collections.Counter(r["vector"] for r in rows)
        o.write("- vector unit: %s\n" % ", ".join("%s (%d)" % kv for kv in vc.most_common()))
        o.write("- Zicboz: %d of %d; cboz block sizes seen: %s\n" % (sum(r["zicboz"] for r in rows), n,
                ", ".join(sorted(set(str(x) for r in rows for x in r["cboz"]))) or "none"))
        uc = collections.Counter(r["uart"] for r in rows)
        o.write("- UART base address: %d distinct values\n" % len(uc))
    print("wrote", out, len(rows), "device trees")

if __name__ == "__main__":
    main()
