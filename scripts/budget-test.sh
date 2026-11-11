#!/bin/zsh
# A bounded performance baseline; no disk or native VM changes.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo 'Run with sudo'; exit 1; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || exit 1
[[ -x $ROOT/build/budgetprobe ]] || exit 1
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
cleanup() {
    local result=$?
    trap - EXIT
    sysctl debug.amdhv.takeover=0 || result=1
    [[ $(sysctl -n debug.amdhv.vms) == 0 && $(sysctl -n kern.hv_support) == $SAVED_SUPPORT ]] || result=1
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.takeover=1
sudo -u "${SUDO_USER:-root}" "$ROOT/build/budgetprobe"
