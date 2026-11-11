#!/bin/zsh
# Phase 3 VM/memory gate (no VMRUN). Needs loaded AMDHV >=0.3.0, probes built by make vmprobe.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo 'Run with sudo'; exit 1; }
[[ -x $ROOT/build/vmprobe && -x $ROOT/build/vmprobe-noent ]] || { echo 'Build first: make vmprobe'; exit 1; }
sysctl -n debug.amdhv.vms >/dev/null || { echo 'Reboot into AMDHV 0.3.x first'; exit 1; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 ]] || { echo 'FAIL: takeover already active'; exit 1; }
[[ $# -eq 0 || ( $# -eq 1 && $1 == --rom ) ]] || { echo 'Usage: vm-test.sh [--rom]'; exit 2; }
if [[ ${1:-} == --rom ]]; then
    [[ -x $ROOT/build/romprobe ]] || { echo 'Build first: make romprobe'; exit 1; }
fi
RUNAS=${SUDO_USER:-root}
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
hold_pid=0
cleanup() {
    local result=$?
    trap - EXIT
    if (( hold_pid )); then
        kill $hold_pid 2>/dev/null || true
        wait $hold_pid 2>/dev/null || true
    fi
    # Task teardown can finish just after waitpid returns.
    for i in {1..100}; do
        [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break
        sleep 0.02
    done
    if ! sysctl debug.amdhv.takeover=0; then
        echo 'FAIL: VM targets remain; backend kept active to preserve their callbacks' >&2
        result=1
    fi
    sysctl -n debug.amdhv.log
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.takeover=1
[[ $(sysctl -n kern.hv_support) == 1 ]]
sudo -u "$RUNAS" "$ROOT/build/vmprobe-noent" --deny
sudo -u "$RUNAS" "$ROOT/build/vmprobe"
if [[ ${1:-} == --rom ]]; then
    sudo -u "$RUNAS" "$ROOT/build/romprobe"
    sudo -u "$RUNAS" "$ROOT/build/memoryprobe"
fi
# A second, signed process owns a VM while root asks the driver to detach.
"$ROOT/build/vmprobe" --hold &
hold_pid=$!
for i in {1..100}; do
    [[ $(sysctl -n debug.amdhv.vms) == 1 ]] && break
    sleep 0.01
done
[[ $(sysctl -n debug.amdhv.vms) == 1 ]] || { echo 'FAIL: hold VM did not start'; exit 1; }
if sysctl debug.amdhv.takeover=0; then
    echo 'FAIL: detached with a live VM'; exit 1
fi
[[ $(sysctl -n debug.amdhv.takeover) == 1 ]]
echo 'PASS: detach refused with a live VM'
wait $hold_pid
hold_pid=0
for i in {1..100}; do
    [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break
    sleep 0.02
done
[[ $(sysctl -n debug.amdhv.vms) == 0 ]]
sysctl debug.amdhv.takeover=0
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n kern.hv_support) == $SAVED_SUPPORT ]]
echo 'PASS: phase 3 VM/memory gate + busy detach + cleanup'
