#!/usr/bin/env python3
# factcover (L12): every node and property kernel/fdtgen.c emits into a guest's device tree,
# from the source, so that Table tab:coverage can be checked against it: a property the
# table does not name is a fact the paper has not thought about.
#   python3 tools/factcover.py
import os, re
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kernel", "fdtgen.c")).read()
nodes = re.findall(r'node_begin\(&w, ([^)]*\))?', src)
props = sorted(set(re.findall(r'prop[a-z_0-9]*\(&w, "([^"]*)"', src)))
print("nodes (%d):" % len(nodes))
for n in nodes: print("  " + n.strip())
print("properties (%d):" % len(props))
for p in props: print("  " + p)
tab = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "paper", "sections", "tab-coverage.tex")).read().replace("\\_", "_").replace("\\#", "#")
missing = [p for p in props if p.split(",")[-1] not in tab and p not in tab]
print("properties the table does not name: %s" % (missing or "none"))
