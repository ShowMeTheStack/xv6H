#!/usr/bin/env python3
#
# the reference archives user/archtest reads.
#
# these are written by Python's tarfile and gzip, which are an
# implementation of the two formats that has nothing to do with ours.
# that is the whole point: archtest passing means xv6's tar and gzip
# read what someone else wrote, not merely what they wrote themselves.
#
# usage: tools/mkarchref.py OUTDIR
#
import gzip, io, os, sys, tarfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

# a name too long for ustar's 100-byte field but splittable at a slash,
# so the reference exercises the prefix field
LONGDIR = "a-directory-with-a-deliberately-long-name-for-the-prefix-field"
LONGNAME = "%s/%s/a-file-whose-path-does-not-fit-in-one-hundred-bytes" % (
    LONGDIR, LONGDIR)


def member(tf, name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    tf.addfile(info, io.BytesIO(data))


def directory(tf, name, mode=0o755):
    info = tarfile.TarInfo(name)
    info.type = tarfile.DIRTYPE
    info.mode = mode
    info.mtime = 0
    tf.addfile(info)


def symlink(tf, name, target):
    info = tarfile.TarInfo(name)
    info.type = tarfile.SYMTYPE
    info.linkname = target
    info.mtime = 0
    tf.addfile(info)


# ---- ref.tar.gz: ustar, the format everything can read ----

buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
    directory(tf, "ref")
    member(tf, "ref/greeting", b"a file from GNU tar\n")
    member(tf, "ref/longname", (LONGNAME + "\n").encode())
    directory(tf, "ref/deep")
    directory(tf, "ref/deep/down")
    member(tf, "ref/deep/down/here", b"nested\n")
    directory(tf, "ref/" + LONGDIR)
    directory(tf, "ref/%s/%s" % (LONGDIR, LONGDIR))
    member(tf, "ref/" + LONGNAME, b"the prefix field works\n")
    symlink(tf, "ref/pointer", "greeting")
    member(tf, "ref/executable", b"#!/bin/sh\necho ran\n", mode=0o755)
with open(os.path.join(out, "ref.tar.gz"), "wb") as f:
    f.write(gzip.compress(buf.getvalue(), 9))

# ---- pax.tar.gz: the extended headers a modern tar writes ----
# one path component longer than a hundred bytes, which ustar's prefix
# split cannot represent, so tarfile has to emit an 'x' header

PAXNAME = "pax/" + ("z" * 130)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tf:
    directory(tf, "pax")
    member(tf, PAXNAME, b"a pax extended header named this\n")
    member(tf, "pax/plain", b"an ordinary member beside it\n")
    symlink(tf, "pax/" + ("y" * 130), "plain")
with open(os.path.join(out, "pax.tar.gz"), "wb") as f:
    f.write(gzip.compress(buf.getvalue(), 6))

# ---- gnu.tar.gz: GNU's own long-name blocks ----

buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
    directory(tf, "gnu")
    member(tf, "gnu/" + ("w" * 130), b"a GNU LongLink named this\n")
    member(tf, "gnu/plain", b"an ordinary member beside it\n")
with open(os.path.join(out, "gnu.tar.gz"), "wb") as f:
    f.write(gzip.compress(buf.getvalue(), 1))

# ---- evil.tar: a member that tries to write outside the directory ----
# extracting this must not create /tmp/pwned

buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
    member(tf, "../../../../../../tmp/pwned", b"escaped\n")
    member(tf, "innocent", b"this one is fine\n")
with open(os.path.join(out, "evil.tar"), "wb") as f:
    f.write(buf.getvalue())

# ---- levels.gz: every deflate level, so all three block types appear ----
# level 0 is stored blocks, 1 is fast matching, 9 is the deepest search

data = (b"the quick brown fox jumps over the lazy dog. " * 400 +
        bytes(range(256)) * 40 + b"\n")
for level in (0, 1, 6, 9):
    with open(os.path.join(out, "level%d.gz" % level), "wb") as f:
        f.write(gzip.compress(data, level))
with open(os.path.join(out, "levels.txt"), "wb") as f:
    f.write(data)

print("wrote reference archives to %s" % out)
