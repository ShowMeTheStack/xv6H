#!/usr/bin/env python3
#
# the manual, as one file.
#
# man/NAME.1 is a page; this concatenates them into a single database
# with a marker before each, because a hundred and sixty-four files of
# two kilobytes each would be a hundred and sixty-four inodes and a
# hundred and sixty-four block reads to find one page. The database is
# read by user/man.c and by the C library's answer to --help
# (user/libc/help.c), which both look for the marker line.
#
#   @@NAME
#   <the page>
#
# and one entry at the end, @@whatis, holding the one-line description
# of every page for `man -k' and for `man' with no arguments. Those
# lines come from each page's NAME section, so the index cannot drift
# from the pages: there is only one copy of the words.
#
import os, re, sys


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: mkman.py OUT man/NAME.1...\n")
        return 1
    out, pages = argv[1], argv[2:]
    entries, whatis = [], []
    for p in sorted(pages):
        name = os.path.basename(p)
        name = name[:name.rindex(".")]
        text = open(p).read().rstrip("\n")
        m = re.search(r"^NAME\n\s*(\S+)\s+-\s+(.*)$", text, re.M)
        if not m:
            sys.stderr.write("%s: no NAME line of the form `name - what it is'\n" % p)
            return 1
        if m.group(1) != name:
            sys.stderr.write("%s: NAME says %s\n" % (p, m.group(1)))
            return 1
        if "SYNOPSIS" not in text:
            sys.stderr.write("%s: no SYNOPSIS\n" % p)
            return 1
        entries.append("@@%s\n%s\n" % (name, text))
        whatis.append("%-18s %s" % (name, m.group(2)))
    with open(out, "w") as f:
        for e in entries:
            f.write(e)
        f.write("@@whatis\n%s\n" % "\n".join(whatis))
    sys.stderr.write("man: %d pages, %d bytes\n"
                     % (len(entries), os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
