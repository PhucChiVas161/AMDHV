#!/bin/sh
set -eu
apk add --no-cache util-linux >/dev/null
count=$(nproc)
echo "CPU_AFFINITY_WORKLOAD count=$count"
pids=""
for cpu in $(seq 0 $((count-1)));do
 taskset -c "$cpu" sh -c 'dd if=/dev/zero bs=1048576 count=32 2>/dev/null | sha256sum' > "/tmp/cpu-$cpu.sha" &
 pids="$pids $!"
done
for pid in $pids;do wait "$pid" || exit 1;done
for cpu in $(seq 0 $((count-1)));do
 grep -qx '83ee47245398adee79bd9c0a8bc57b821e92aba10f5f9ade8a5d1fae4d8c4302  -' "/tmp/cpu-$cpu.sha" || exit 1
 printf 'CPU%s ' "$cpu";cat "/tmp/cpu-$cpu.sha"
done
echo FIXVM_AFFINITY_HASH_PASS
