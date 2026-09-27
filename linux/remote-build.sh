#!/bin/bash
# build the Linux guest (kernel, initramfs, machrev.ko, disk image) on the
# build box from the sources in this directory, then fetch the artifacts.
#   linux/remote-build.sh            # rebuild the kernel and module
set -e
HOST=${HOST:-build@buildbox}
cd "$(dirname "$0")"
ssh $HOST "mkdir -p xv6h-linux/src/machrev xv6h-linux/src/patches"
rsync -q -a --delete machrev/ $HOST:xv6h-linux/src/machrev/
rsync -q -a --delete patches/ $HOST:xv6h-linux/src/patches/
scp -q build.sh zerocheck.c $HOST:xv6h-linux/src/
ssh $HOST "REBUILD=1 bash ~/xv6h-linux/src/build.sh" 2>&1 | tail -${TAIL:-15}
./fetch.sh
