#!/bin/zsh
# Default validates configuration; --start tests disposable EFI. Does not modify Podman machines.
set -euo pipefail
ROOT=${0:A:h:h}
USE_FAST=0
if [[ ${1:-} == --fast ]];then USE_FAST=1;shift;fi
[[ $EUID -eq 0 ]] || { echo 'Run with sudo'; exit 1; }
[[ -x $ROOT/build/vzprobe ]] || { echo 'Build first: make vzprobe'; exit 1; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || {
    echo 'Backend must be idle'; exit 1;
}
[[ $# -eq 0 || ( $# -eq 1 && $1 == --start ) ||
   ( $# -eq 3 && $1 == --start && $2 == --seconds && $3 == <1-300> ) ||
   ( $# -eq 3 && $1 == --linux && -f $2 && -f $3 ) ||
   ( $# -eq 5 && $1 == --linux && -f $2 && -f $3 && $4 == --seconds && $5 == <1-300> ) ]] || {
    echo 'Usage: vz-probe.sh [--fast] [--start [--seconds 1..300] | --linux KERNEL INITRD [--seconds 1..300]]'; exit 2;
}
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
SAVED_FAST=0
if (( USE_FAST ));then SAVED_FAST=$(sysctl -n debug.amdhv.fast);fi
cleanup() {
    local result=$?
    trap - EXIT
    for i in {1..300}; do
        [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break
        sleep 0.1
    done
    sysctl debug.amdhv.takeover=0 || result=1
    if (( USE_FAST ));then sysctl debug.amdhv.fast=$SAVED_FAST || result=1;fi
    sysctl -n debug.amdhv.log
    [[ $(sysctl -n kern.hv_support) == $SAVED_SUPPORT && $(sysctl -n debug.amdhv.vms) == 0 ]] || result=1
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.log=clear >/dev/null
if (( USE_FAST ));then sysctl debug.amdhv.fast=1;fi
sysctl debug.amdhv.takeover=1
sudo -u "${SUDO_USER:-root}" env FIXVM_LINUX_EXTRA_CMDLINE="${FIXVM_LINUX_EXTRA_CMDLINE:-}" "$ROOT/build/vzprobe" "$@"
