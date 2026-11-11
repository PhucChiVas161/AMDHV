#!/bin/zsh
set -euo pipefail
ROOT=/Users/phucduong/Desktop/FixVM
[[ $EUID == 0 && $(sysctl -n debug.amdhv.vms) == 0 && $(sysctl -n debug.amdhv.takeover) == 0 ]] || exit 2
saved_fast=$(sysctl -n debug.amdhv.fast)
saved_parallel=$(sysctl -n debug.amdhv.parallel)
typeset -a pids
cleanup(){
 local result=$?;trap - EXIT
 for pid in "${pids[@]}";do if kill -0 $pid 2>/dev/null;then kill -TERM $pid || true;fi;done
 for pid in "${pids[@]}";do wait $pid 2>/dev/null || true;done
 for i in {1..200};do [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break;sleep 0.05;done
 sysctl debug.amdhv.takeover=0 || result=1
 sysctl debug.amdhv.fast=$saved_fast || result=1
 sysctl debug.amdhv.parallel=$saved_parallel || result=1
 sysctl -n debug.amdhv.log
 [[ $(sysctl -n debug.amdhv.vms) == 0 ]] || result=1
 exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.parallel=1
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.takeover=1
sudo -u "$SUDO_USER" "$ROOT/build/smpprobe" > "$ROOT/research/runs/0.3.40-multivm-smp.log" 2>&1 &
pids+=($!)
sudo -u "$SUDO_USER" "$ROOT/build/aplongprobe" > "$ROOT/research/runs/0.3.40-multivm-ap.log" 2>&1 &
pids+=($!)
peak=0
for i in {1..200};do
 live=$(sysctl -n debug.amdhv.vms)
 (( live > peak )) && peak=$live
 if ! kill -0 $pids[1] 2>/dev/null && ! kill -0 $pids[2] 2>/dev/null;then break;fi
 sleep 0.005
done
result=0
for pid in "${pids[@]}";do wait $pid || result=1;done
pids=()
cat "$ROOT/research/runs/0.3.40-multivm-smp.log" "$ROOT/research/runs/0.3.40-multivm-ap.log"
print "Multi-VM peak live=$peak"
[[ $result == 0 && $peak == 2 ]] || exit 1
print 'PASS: two VM tasks with distinct code/page tables, concurrent AP/SMP runs and mapping writers; all guests destroyed'
