#!/bin/bash
# copy the build artifacts from the build box into linux/ here.
set -e
HOST=${HOST:-build@buildbox}
cd "$(dirname "$0")"
scp -q $HOST:xv6h-linux/out/Image-hf ./Image
scp -q $HOST:xv6h-linux/out/Image-hf.config ./Image.config
scp -q $HOST:xv6h-linux/out/linux.img ./linux.img
scp -q $HOST:xv6h-linux/out/Image-nofpu ./Image-nofpu 2>/dev/null || true
ls -la Image linux.img Image-nofpu 2>/dev/null
