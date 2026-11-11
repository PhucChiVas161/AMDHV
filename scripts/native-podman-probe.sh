#!/bin/zsh
# Isolated profile and clone only. Starts/stops through the real applehv provider.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo 'Run with sudo';exit 1; }
/usr/bin/caffeinate -i -w $$ &
print "Idle-sleep guard PID=$! (until probe exits)"
[[ $# -ge 2 ]] || { echo 'Usage: native-podman-probe.sh PROFILE NAME [USER_WORKLOAD_SCRIPT]';exit 2; }
PROFILE=$1;NAME=$2;WORKLOAD=${3:-}
PARALLEL=${FIXVM_PARALLEL:-0}
[[ $PARALLEL == 0 || $PARALLEL == 1 ]] || { echo 'FIXVM_PARALLEL must be 0 or 1';exit 2; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || { echo 'Backend must be idle';exit 1; }
# Separate runtime PID/socket files too: XDG config/data alone still share
# $TMPDIR/podman/gvproxy.pid with the user's default Podman machine.
# Keep the prefix short enough for macOS Unix-domain socket path limits.
USER_TMPDIR=$(sudo -u "${SUDO_USER:-root}" /usr/bin/mktemp -d /private/tmp/fixvm-podman.XXXXXXXX)/
print "Isolated Podman runtime: $USER_TMPDIR"
RUN=(sudo -u "${SUDO_USER:-root}" env "TMPDIR=$USER_TMPDIR" "XDG_CONFIG_HOME=$PROFILE/config" "XDG_DATA_HOME=$PROFILE/data" CONTAINERS_MACHINE_PROVIDER=applehv)
PODMAN=("${RUN[@]}" /opt/podman/bin/podman)
SAVED_FAST=$(sysctl -n debug.amdhv.fast)
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
SAVED_PARALLEL=$(sysctl -n debug.amdhv.parallel)
cleanup() {
    local result=$?
    trap - EXIT
    "${PODMAN[@]}" machine stop "$NAME" || result=1
    for i in {1..300};do [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break;sleep 0.1;done
    sysctl debug.amdhv.takeover=0 || result=1
    sysctl debug.amdhv.fast=$SAVED_FAST || result=1
    sysctl debug.amdhv.parallel=$SAVED_PARALLEL || result=1
    sysctl -n debug.amdhv.log
    [[ $(sysctl -n kern.hv_support) == $SAVED_SUPPORT && $(sysctl -n debug.amdhv.vms) == 0 ]] || result=1
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.parallel=$PARALLEL
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.takeover=1
print 'TIMING: provider machine start (includes guest boot and provider readiness checks)'
/usr/bin/time -p "${PODMAN[@]}" machine start "$NAME"
"${PODMAN[@]}" --connection "$NAME-root" info --format '{{.Host.Arch}} {{.Host.CPUs}} {{.Store.GraphDriverName}}'
if [[ -n $WORKLOAD ]];then "${RUN[@]}" "$WORKLOAD" "$NAME";fi
