#!/usr/bin/env python3
# summary of an ILA capture of Rocket's ports (fpga/zedboard/ila.tcl): reads, answers, device writes
import sys
def ila_summary(path):
    """what Rocket did in the 4096 cycles from its reset: reads and their answers, device writes"""
    import csv
    try:
        rows = list(csv.reader(open(path)))
    except OSError as e:
        return "no ILA capture: %s" % e
    hdr = rows[0]; data = rows[2:] if len(rows) > 2 and rows[1][0].startswith("Radix") else rows[1:]
    # the probe's own name, its last component without the bit range ("RVALID" is in "ARVALID")
    base = lambda h: h.split("/")[-1].split("[")[0]
    col = lambda key: next((i for i, h in enumerate(hdr) if base(h) == key), None)
    names = {"rstgate_Res": "rstgate_Res", "ARVALID": "rocket_m_mem_axi_ARVALID", "ARREADY": "rocket_m_mem_axi_ARREADY",
             "ARADDR": "rocket_m_mem_axi_ARADDR", "RVALID": "rocket_m_mem_axi_RVALID", "RREADY": "rocket_m_mem_axi_RREADY",
             "RRESP": "rocket_m_mem_axi_RRESP", "mmio_axi_AWVALID": "rocket_m_mmio_axi_AWVALID",
             "mmio_axi_AWREADY": "rocket_m_mmio_axi_AWREADY", "mmio_axi_AWADDR": "rocket_m_mmio_axi_AWADDR",
             "mmio_axi_WDATA": "rocket_m_mmio_axi_WDATA", "mmio_axi_WVALID": "rocket_m_mmio_axi_WVALID",
             "mmio_axi_BRESP": "rocket_m_mmio_axi_BRESP", "mmio_axi_BVALID": "rocket_m_mmio_axi_BVALID",
             "AWVALID": "rocket_m_mem_axi_AWVALID", "BRESP": "rocket_m_mem_axi_BRESP", "BVALID": "rocket_m_mem_axi_BVALID"}
    c = {k: col(n) for k, n in names.items()}
    def v(r, k, base=16):
        i = c.get(k)
        return int(r[i], base) if i is not None and r[i] not in ("", "X") else 0
    out = ["ILA: %d samples; probes missing: %s" % (len(data), [k for k, i in c.items() if i is None])]
    reads = [(n, v(r, "ARADDR")) for n, r in enumerate(data) if v(r, "ARVALID") and v(r, "ARREADY")]
    resps = [(n, v(r, "RRESP")) for n, r in enumerate(data) if v(r, "RVALID") and v(r, "RREADY")]
    mw = [(n, v(r, "mmio_axi_AWADDR"), v(r, "mmio_axi_WDATA")) for n, r in enumerate(data) if v(r, "mmio_axi_WVALID")]
    rst = [n for n, r in enumerate(data) if v(r, "rstgate_Res")]
    out.append("reset released at sample %s" % (rst[0] if rst else "never"))
    out.append("memory reads accepted: %d; first: %s" % (len(reads), ", ".join("%d:0x%08x" % x for x in reads[:12])))
    out.append("read responses: %d; RRESP values %s" % (len(resps), sorted(set(x[1] for x in resps))))
    out.append("device writes: %d; first: %s" % (len(mw), ", ".join("%d:0x%x=%x" % x for x in mw[:12])))
    chars = "".join(chr(w & 0xff) for _, a, w in mw if (a & 0xfff) == 0 and 32 <= (w & 0xff) < 127)
    out.append("UART bytes in the window: %r" % chars)
    wr = [n for n, r in enumerate(data) if v(r, "AWVALID")]
    out.append("memory writes started: %d; write responses not OKAY: %d" % (len(wr), sum(1 for r in data if v(r, "BVALID") and v(r, "BRESP"))))
    return "\n".join(out)

if __name__ == '__main__':
    print(ila_summary(sys.argv[1]))
