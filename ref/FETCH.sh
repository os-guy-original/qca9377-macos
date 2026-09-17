#!/bin/sh
# FETCH.sh - reconstruct ref/linux-ath10k from the pinned upstream tarball.
# Run from this directory. Requires: curl, tar with xz support, sha256sum.
set -eu

PINNED_SHA256=8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03
URL=https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz

echo "== downloading $(basename "$URL") (about 160 MB) =="
curl -fL -o linux-7.2.3.tar.xz "$URL"

echo "== verifying sha256 (pinned) =="
echo "$PINNED_SHA256  linux-7.2.3.tar.xz" | sha256sum -c -

echo "== extracting ath/ath10k reference files =="
tar -xJf linux-7.2.3.tar.xz \
  --wildcards \
  'linux-7.2.3/drivers/net/wireless/ath/ath10k/*' \
  'linux-7.2.3/drivers/net/wireless/ath/ath.h' \
  'linux-7.2.3/drivers/net/wireless/ath/regd.c' \
  'linux-7.2.3/drivers/net/wireless/ath/regd.h' \
  'linux-7.2.3/drivers/net/wireless/ath/trace.c' \
  'linux-7.2.3/drivers/net/wireless/ath/trace.h' \
  'linux-7.2.3/drivers/net/wireless/ath/Kconfig' \
  'linux-7.2.3/drivers/net/wireless/ath/Makefile'

mkdir -p linux-ath10k
mv linux-7.2.3/drivers/net/wireless/ath/* linux-ath10k/
rm -rf linux-7.2.3 linux-7.2.3.tar.xz

echo "== done: $(find linux-ath10k -name '*.c' | wc -l) ath10k .c files extracted =="
echo "License check (all files must be ISC):"
grep -rhoE "SPDX-License-Identifier: [A-Za-z0-9_-]+" linux-ath10k/ath10k/ | sort | uniq -c
