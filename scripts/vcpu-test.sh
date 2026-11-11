#!/bin/zsh
# Tests lifecycle first, then bounded real-mode SVM through Hypervisor.framework.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo 'Run with sudo'; exit 1; }
[[ -x $ROOT/build/vcpuprobe ]] || { echo 'Build first: make vcpuprobe'; exit 1; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || {
    echo 'FAIL: backend already active'; exit 1;
}
if [[ ${1:-} == --modes ]]; then
    [[ -x $ROOT/build/eventprobe && -x $ROOT/build/debugprobe && -x $ROOT/build/modeprobe && -x $ROOT/build/tscprobe && -x $ROOT/build/msrprobe && -x $ROOT/build/nativeinitprobe && -x $ROOT/build/interruptprobe && -x $ROOT/build/resetprobe && -x $ROOT/build/descriptorprobe && -x $ROOT/build/controlprobe && -x $ROOT/build/longtransitionprobe && -x $ROOT/build/flagsprobe && -x $ROOT/build/tlbprobe && -x $ROOT/build/budgetprobe && -x $ROOT/build/nxprobe && -x $ROOT/build/pagingcontrolprobe && -x $ROOT/build/fpprobe && -x $ROOT/build/ioprobe ]] || { echo 'Build first: make modeprobe'; exit 1; }
    kmutil showloaded 2>/dev/null | grep -q 'com.fixvm.driver.AMDHV (0.3.42)' || {
        echo 'Mode gate requires loaded AMDHV 0.3.42'; exit 1;
    }
elif [[ $# -gt 0 ]]; then
    echo 'Usage: vcpu-test.sh [--modes]'; exit 2
fi
[[ $(sysctl -n debug.amdhv.fast) == 0 ]] || { echo 'Step gate requires debug.amdhv.fast=0';exit 1; }
RUNAS=${SUDO_USER:-root}
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
cleanup() {
    local result=$?
    trap - EXIT
    for i in {1..100}; do
        [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break
        sleep 0.02
    done
    if ! sysctl debug.amdhv.takeover=0; then result=1; fi
    sysctl -n debug.amdhv.log
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sync
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.takeover=1
sudo -u "$RUNAS" "$ROOT/build/vcpuprobe"
sudo -u "$RUNAS" "$ROOT/build/vcpuprobe" --run
if [[ ${1:-} == --modes ]]; then
    sudo -u "$RUNAS" "$ROOT/build/nativeinitprobe"
    sudo -u "$RUNAS" "$ROOT/build/modeprobe"
    sudo -u "$RUNAS" "$ROOT/build/tscprobe"
    sudo -u "$RUNAS" "$ROOT/build/msrprobe"
    sudo -u "$RUNAS" "$ROOT/build/interruptprobe"
    sudo -u "$RUNAS" "$ROOT/build/resetprobe"
    sudo -u "$RUNAS" "$ROOT/build/descriptorprobe"
    sudo -u "$RUNAS" "$ROOT/build/systemprobe"
    sudo -u "$RUNAS" "$ROOT/build/controlprobe"
    sudo -u "$RUNAS" "$ROOT/build/longtransitionprobe"
    sudo -u "$RUNAS" "$ROOT/build/ioprobe"
    sudo -u "$RUNAS" "$ROOT/build/fpprobe"
    sudo -u "$RUNAS" "$ROOT/build/pagingcontrolprobe"
    sudo -u "$RUNAS" "$ROOT/build/nxprobe"
    sudo -u "$RUNAS" "$ROOT/build/flagsprobe"
    sudo -u "$RUNAS" "$ROOT/build/cacheprobe"
    sudo -u "$RUNAS" "$ROOT/build/tlbprobe"
    sudo -u "$RUNAS" "$ROOT/build/budgetprobe"
    sudo -u "$RUNAS" "$ROOT/build/debugprobe"
    sudo -u "$RUNAS" "$ROOT/build/eventprobe"
    sudo -u "$RUNAS" "$ROOT/build/eventprobe" --high
fi
sysctl debug.amdhv.takeover=0
[[ $(sysctl -n debug.amdhv.vms) == 0 && $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n kern.hv_support) == $SAVED_SUPPORT ]]
echo 'PASS: vCPU lifecycle + bounded SVM + cleanup'
