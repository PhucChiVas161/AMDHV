#!/bin/zsh
set -euo pipefail
NAME=$1
/opt/podman/bin/podman machine ssh "$NAME" -- sudo sh -eu -c '
cat /proc/cmdline
if grep -qw tsc=reliable /proc/cmdline;then echo "FAIL: diagnostic argument still active";exit 1;fi
rpm-ostree kargs > /var/tmp/fixvm-kargs-after-tsc-diagnostic
if grep -qw tsc=reliable /var/tmp/fixvm-kargs-after-tsc-diagnostic;then echo "FAIL: diagnostic argument still queued";exit 1;fi
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
echo "PASS: default guest boot arguments restored and active"
'
/Users/phucduong/Desktop/FixVM/research/disposable/verify-podman-reboot-0.3.38.zsh "$NAME"
