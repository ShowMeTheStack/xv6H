#!/bin/bash
# build user/https.c (the TLS client) as a static Linux binary with the
# musl cross compiler on the build box, for the Linux guest's initramfs.
# run this before linux/build.sh (which picks up out/https if it exists).
set -e
HOST=${HOST:-build@buildbox}
cd "$(dirname "$0")/.."
tar czf - tls/bearssl/src tls/bearssl/inc tls/trust_anchors.c user/https.c | ssh $HOST 'set -e
  mkdir -p ~/xv6h-linux/https && cd ~/xv6h-linux/https && rm -rf tls user && tar xzf -
  CC=$HOME/xv6h-linux/musl/bin/riscv64-linux-musl-gcc
  # the build date stands in for a clock the guest does not have (see user/https.c).
  # BearSSL must not seed itself from /dev/urandom or getentropy(): both block
  # until the guest kernel has gathered entropy, which takes minutes under
  # emulation; https.c injects getrandom(GRND_INSECURE) bytes instead.
  CFLAGS="-O2 -static -march=rv64gc -mabi=lp64d -Wall -Itls/bearssl/inc -Itls/bearssl/src -DBR_64=1 -DBR_USE_UNIX_TIME=1 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DTLS_BUILD_SECS=$(date -u +%s)UL"
  $CC $CFLAGS -o https user/https.c tls/trust_anchors.c tls/bearssl/src/settings.c tls/bearssl/src/*/*.c
  $HOME/xv6h-linux/musl/bin/riscv64-linux-musl-strip https; mkdir -p ~/xv6h-linux/out; cp https ~/xv6h-linux/out/https
  ls -la https'
