#!/bin/sh
set -eu
dd if=/dev/zero of=/tmp/fixvm-input bs=1048576 count=32 2>/dev/null
sync
expected=83ee47245398adee79bd9c0a8bc57b821e92aba10f5f9ade8a5d1fae4d8c4302
for round in $(seq 1 64);do
 actual=$(sha256sum /tmp/fixvm-input | cut -d ' ' -f 1)
 [ "$actual" = "$expected" ] || { echo "FAIL: CPU$FIXVM_CPU round$round digest=$actual";exit 1; }
done
printf 'PASS: CPU%s 64 verified SHA256 reads of 32MiB; disk write/sync/read verified\n' "$FIXVM_CPU"
