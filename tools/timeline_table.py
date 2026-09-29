# timeline_table (L15): the crossing's outage decomposed, from the host lines the two sides
# print ("crossing, this side: stopped +0, last page +A us, taken by the far end +B us,
# committed +C us" on the source; "arrival timeline: stream done +0, resumed +D us, first
# instruction +E us" and "revision asked +F us, adopted +G us" on the destination), per
# test name, over a run directory's logs. Medians and 90th percentiles in ms.
#   python3 tools/timeline_table.py DIR [DIR...]      (a run's qlogs/ or a single test-hyp.log)
import os, re, sys, glob, statistics
S = re.compile(r"crossing, this side: stopped \+0, last page \+(\d+) us, taken by the far end \+(\d+) us, committed \+(\d+) us")
A = re.compile(r"arrival timeline: stream done \+0, resumed \+(\d+) us, first instruction \+(\d+) us")
R = re.compile(r"arrival timeline: revision asked \+(\d+) us, adopted \+(\d+) us")
T = re.compile(r"^== (snap-[a-z0-9-]+)$", re.M)
rows = {}
def take(name, text):
    d = rows.setdefault(name, {"lastpage": [], "taken": [], "committed": [], "first": [], "asked": [], "adopted": []})
    for m in S.finditer(text):
        d["lastpage"].append(int(m.group(1)) / 1000); d["taken"].append(int(m.group(2)) / 1000); d["committed"].append(int(m.group(3)) / 1000)
    for m in A.finditer(text): d["first"].append(int(m.group(2)) / 1000)
    for m in R.finditer(text): d["asked"].append(int(m.group(1)) / 1000); d["adopted"].append(int(m.group(2)) / 1000)
for arg in sys.argv[1:]:
    files = glob.glob(os.path.join(arg, "*.out")) if os.path.isdir(arg) else [arg]
    for f in files:
        text = open(f, errors="replace").read()
        # a qlog is one test; a test-hyp.log has many, split by the "== name" headers
        parts = T.split(text)
        if len(parts) > 1:
            for i in range(1, len(parts), 2): take(parts[i], parts[i + 1])
        else:
            name = os.path.basename(f).rsplit(".", 2)[0]; take(name, text)
def q(v, p): v = sorted(v); return v[min(len(v) - 1, int(p * (len(v) - 1)))] if v else float("nan")
print("%-34s %4s %10s %10s %10s | %10s %10s %10s" % ("test", "n", "last page", "taken", "committed", "first insn", "asked", "adopted"))
print("%-34s %4s %10s %10s %10s | %10s %10s %10s" % ("", "", "med/p90", "med/p90", "med/p90", "med/p90", "med/p90", "med/p90"))
for name in sorted(rows):
    d = rows[name]
    if not d["committed"] and not d["first"]: continue
    n = max(len(d["committed"]), len(d["first"]))
    f = lambda k: ("%.0f/%.0f" % (statistics.median(d[k]), q(d[k], 0.9))) if d[k] else "-"
    print("%-34s %4d %10s %10s %10s | %10s %10s %10s" % (name, n, f("lastpage"), f("taken"), f("committed"), f("first"), f("asked"), f("adopted")))
