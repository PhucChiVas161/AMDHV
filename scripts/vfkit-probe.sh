#!/bin/zsh
# Disposable vfkit diagnostics. Use separate test disks and EFI variable stores.
# Power off the guest, or terminate its vfkit process; cleanup waits for VM release.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo 'Run with sudo'; exit 1; }
[[ $# -ge 2 ]] || { echo 'Usage: vfkit-probe.sh LOGFILE VFKIT_ARGS...'; exit 2; }
LOGFILE=$1;shift
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || { echo 'Backend must be idle'; exit 1; }
SAVED_FAST=$(sysctl -n debug.amdhv.fast)
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
cleanup() {
    local result=$?
    trap - EXIT
    for i in {1..300};do [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break;sleep 0.1;done
    sysctl debug.amdhv.takeover=0 || result=1
    sysctl debug.amdhv.fast=$SAVED_FAST || result=1
    sysctl -n debug.amdhv.log > "$LOGFILE.amdhv.log"
    [[ $(sysctl -n kern.hv_support) == $SAVED_SUPPORT && $(sysctl -n debug.amdhv.vms) == 0 ]] || result=1
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.takeover=1
sudo -u "${SUDO_USER:-root}" /usr/bin/script -q "$LOGFILE" /opt/podman/bin/vfkit "$@"
