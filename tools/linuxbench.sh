#!/bin/bash
#
# schedbench on Linux, on the same QEMU, from the workstation:
#
#   tools/linuxbench.sh [-t secs] [-c cpus] [test ...]
#
# it copies the benchmark and the runner to the build box, where the
# kernel, the cross compiler and qemu live, and prints what comes back.
#
set -e
HOST=${HOST:-build@buildbox}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ssh -o BatchMode=yes $HOST "mkdir -p ~/xv6h-linuxbench/tools ~/xv6h-linuxbench/user"
scp -q "$HERE/tools/linuxbench.py" "$HOST:~/xv6h-linuxbench/tools/"
scp -q "$HERE/user/schedbench.c" "$HOST:~/xv6h-linuxbench/user/"
ssh -o BatchMode=yes $HOST "python3 ~/xv6h-linuxbench/tools/linuxbench.py $*"
