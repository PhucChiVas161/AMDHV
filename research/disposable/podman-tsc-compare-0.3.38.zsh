#!/bin/zsh
set -euo pipefail
NAME=$1
print 'DIAGNOSTIC ONLY: tsc=reliable guest; does not establish default Podman compatibility'
/opt/podman/bin/podman machine ssh "$NAME" -- sudo sh -eu -c '
cat /proc/cmdline
grep -qw tsc=reliable /proc/cmdline
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
dmesg | grep -E "tsc:|clocksource:|TSC|smp: Brought" || true
rpm-ostree kargs --delete=tsc=reliable
echo "RESTORED: queued default boot arguments for next guest boot"
'
/Users/phucduong/Desktop/FixVM/research/disposable/verify-podman-reboot-0.3.38.zsh "$NAME"
/Users/phucduong/Desktop/FixVM/research/disposable/verify-clean-podman-0.3.37.zsh "$NAME"
