#!/bin/zsh
set -euo pipefail
NAME=$1
/opt/podman/bin/podman machine ssh "$NAME" -- sudo sh -eu -c '
echo "=== Original guest clocks and boot arguments ==="
cat /proc/cmdline
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
dmesg | grep -E "tsc:|clocksource:|TSC|smp: Brought" || true
rpm-ostree kargs > /var/tmp/fixvm-kargs-before-tsc-diagnostic
cat /var/tmp/fixvm-kargs-before-tsc-diagnostic
if grep -qw "tsc=reliable" /var/tmp/fixvm-kargs-before-tsc-diagnostic; then
 echo "Refusing to overwrite original-arguments backup"; exit 1
fi
rpm-ostree kargs --append=tsc=reliable
echo "DIAGNOSTIC ONLY: disposable VM next boot uses tsc=reliable; restore after comparison"
'
