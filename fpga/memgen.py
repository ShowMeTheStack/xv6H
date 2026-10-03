#!/usr/bin/env python3
# B.0: block-RAM implementations for the memories firtool leaves as external macros
# (--repl-seq-mem). One read-write port (mrw), synchronous read, a write mask of mask_gran bits
# per lane: written in the per-lane form Vivado infers as a byte/lane-write block RAM.
#   memgen.py mems.conf > mems.v
import sys
out = []
for line in open(sys.argv[1]):
    f = line.split()
    if not f: continue
    kv = dict(zip(f[0::2], f[1::2]))
    name, depth, width = kv["name"], int(kv["depth"]), int(kv["width"])
    ports = kv["ports"]; gran = int(kv.get("mask_gran", width))
    assert ports in ("mrw", "rw"), (name, ports)
    lanes = width // gran; aw = max(1, (depth - 1).bit_length())
    masked = ports == "mrw"
    out.append("// %s: %d x %d, %d lane(s) of %d bits\n(* keep_hierarchy = \"yes\" *)" % (name, depth, width, lanes, gran))
    out.append("module %s(" % name)
    out.append("  input  [%d:0] RW0_addr, input RW0_en, input RW0_clk, input RW0_wmode," % (aw - 1))
    out.append("  input  [%d:0] RW0_wdata, output reg [%d:0] RW0_rdata%s);" % (width - 1, width - 1, (", input [%d:0] RW0_wmask" % (lanes - 1)) if masked else ""))
    out.append("  (* ram_style = \"block\" *) reg [%d:0] ram [0:%d];" % (width - 1, depth - 1))
    out.append("  integer i;")
    out.append("  always @(posedge RW0_clk) begin")
    out.append("    if (RW0_en) begin")
    out.append("      if (RW0_wmode) begin")
    if masked:
        out.append("        for (i = 0; i < %d; i = i + 1)" % lanes)
        out.append("          if (RW0_wmask[i]) ram[RW0_addr][i*%d +: %d] <= RW0_wdata[i*%d +: %d];" % (gran, gran, gran, gran))
    else:
        out.append("        ram[RW0_addr] <= RW0_wdata;")
    out.append("      end else")
    out.append("        RW0_rdata <= ram[RW0_addr];")
    out.append("    end")
    out.append("  end")
    out.append("endmodule\n")
print("\n".join(out))
