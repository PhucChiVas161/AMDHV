#!/bin/sh
# Rerunnable Linux baseline: ./research/linux/collect.sh > research/linux/<date>-<kernel>-sysinfo.txt
# No root needed. Builds and runs the KVM reference probe.
cd "$(dirname "$0")"
echo "# date: $(date -Iseconds)"
echo "# uname: $(uname -a)"
grep PRETTY_NAME /etc/os-release
lscpu | grep -E 'Model name|^CPU\(s\)|Thread'
grep -m1 microcode /proc/cpuinfo
echo "svm flags: $(grep -o -wE 'svm|npt|nrip_save|lbrv|svm_lock|tsc_scale|vmcb_clean|flushbyasid|decodeassists|pausefilter|pfthreshold|avic|x2avic|v_vmsave_vmload|vgif|vnmi|v_spec_ctrl' /proc/cpuinfo | sort -u | tr '\n' ' ')"
echo "kvm_amd: $(cd /sys/module/kvm_amd/parameters && for f in *; do printf '%s=%s ' "$f" "$(cat "$f")"; done)"
ls -l /dev/kvm
journalctl -k -b --no-pager 2>/dev/null | grep -E 'kvm_amd|microcode' | sed 's/^.*kernel: //'
gcc -O2 -Wall -Wextra -o kvm-ref/kvmref kvm-ref/kvmref.c && ./kvm-ref/kvmref
