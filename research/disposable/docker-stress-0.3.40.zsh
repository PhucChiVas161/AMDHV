#!/bin/zsh
set -euo pipefail
ROOT=/Users/phucduong/Desktop/FixVM
D=(/Applications/Docker.app/Contents/Resources/bin/docker -H "unix://$1")
typeset -a names pids
output_dir=$(mktemp -d /private/tmp/fixvm-stress.XXXXXXXX)
print "Stress output: $output_dir"
cleanup(){
 local result=$?;trap - EXIT
 for name in "${names[@]}";do "${D[@]}" rm -f "$name" >/dev/null 2>&1 || true;done
 exit $result
}
trap cleanup EXIT
for cpu in {0..3};do
 name=fixvm-stress040-$$-$cpu;names+=("$name")
 "${D[@]}" run --rm --pull=never --name "$name" --network none --memory 256m --cpuset-cpus "$cpu" -e "FIXVM_CPU=$cpu" -i alpine:3.22 sh -s < "$ROOT/research/disposable/hash-stress-0.3.40.sh" > "$output_dir/cpu$cpu.log" 2>&1 &
 pids+=($!)
done
result=0
for pid in "${pids[@]}";do wait "$pid" || result=1;done
for cpu in {0..3};do cat "$output_dir/cpu$cpu.log";done
(( result == 0 )) || exit 1
"$ROOT/research/disposable/verify-docker-reboot-0.3.38.zsh" "$1"
print 'PASS: four CPU-pinned containers, 256 verified hashes / 8GiB aggregate reads; persisted proof unchanged'
