#!/bin/zsh
set -euo pipefail
ROOT=/Users/phucduong/Desktop/FixVM
/opt/podman/bin/podman machine ssh "$1" -- sudo sh -eu -c '
uname -r; nproc; cat /proc/cmdline
if grep -qw tsc=reliable /proc/cmdline;then echo "FAIL: diagnostic clock argument active";exit 1;fi
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
dmesg | grep -E "clocksource:|tsc:|TSC|smp: Brought" || true
'
"$ROOT/research/disposable/verify-podman-reboot-0.3.38.zsh" "$1"
print 'SCOPE: full default Podman workload follows; nested time measures affinity fixture including container/package overhead'
/usr/bin/time -p "$ROOT/research/disposable/verify-clean-podman-0.3.37.zsh" "$1"
