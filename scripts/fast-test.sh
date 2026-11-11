#!/bin/zsh
# Opt-in host-IRQ/no-TF execution; run the default vCPU gate first after reboot.
set -euo pipefail
ROOT=${0:A:h:h}
USE_PARALLEL=0
if [[ $# == 1 && $1 == --parallel ]];then USE_PARALLEL=1
elif (( $# ));then echo 'Usage: fast-test.sh [--parallel]';exit 2;fi
[[ $EUID -eq 0 ]] || { echo 'Run with sudo';exit 1; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || { echo 'Backend must be idle';exit 1; }
kmutil showloaded 2>/dev/null | grep -q 'com.fixvm.driver.AMDHV (0.3.42)' || { echo 'Requires loaded 0.3.42';exit 1; }
SAVED_FAST=$(sysctl -n debug.amdhv.fast)
SAVED_TRACE=$(sysctl -n debug.amdhv.trace_exits)
SAVED_PARALLEL=$(sysctl -n debug.amdhv.parallel)
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
cleanup() {
    local result=$?
    trap - EXIT
    for i in {1..100};do [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break;sleep 0.02;done
    sysctl debug.amdhv.takeover=0 || result=1
    sysctl debug.amdhv.fast=$SAVED_FAST || result=1
    sysctl debug.amdhv.trace_exits=$SAVED_TRACE || result=1
    sysctl debug.amdhv.parallel=$SAVED_PARALLEL || result=1
    sysctl -n debug.amdhv.log
    [[ $(sysctl -n kern.hv_support) == $SAVED_SUPPORT ]] || result=1
    exit $result
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if sysctl debug.amdhv.parallel=2;then echo 'FAIL: accepted invalid parallel flag';exit 1;fi
sysctl debug.amdhv.parallel=$USE_PARALLEL
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.takeover=1
if sysctl debug.amdhv.fast=0;then echo 'FAIL: changed run mode while traps active';exit 1;fi
[[ $(sysctl -n debug.amdhv.fast) == 1 ]] || { echo 'FAIL: fast mode lost';exit 1; }
if sysctl debug.amdhv.parallel=0;then echo 'FAIL: changed parallel policy while traps active';exit 1;fi
if sysctl debug.amdhv.trace_exits=2;then echo 'FAIL: accepted invalid trace flag';exit 1;fi
sysctl debug.amdhv.trace_exits=1
sudo -u "${SUDO_USER:-root}" "$ROOT/build/fastprobe"
sysctl debug.amdhv.trace_exits=0
[[ $(sysctl -n debug.amdhv.fast) == 1 ]] || { echo 'FAIL: trace toggle changed execution mode';exit 1; }
if (( USE_PARALLEL ));then
    sudo -u "${SUDO_USER:-root}" "$ROOT/build/smpprobe"
else
    sudo -u "${SUDO_USER:-root}" "$ROOT/build/smpprobe" --baseline
fi
sudo -u "${SUDO_USER:-root}" "$ROOT/build/systemprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/eventprobe" --high --hardware
sudo -u "${SUDO_USER:-root}" "$ROOT/build/faultprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/xsaveprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/cplprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/specprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/cacheprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/apresetprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/aptransitionprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/aplongprobe"
sudo -u "${SUDO_USER:-root}" "$ROOT/build/longtransitionprobe" --sce
sudo -u "${SUDO_USER:-root}" "$ROOT/build/nmiprobe"
echo 'PASS: fast no-TF/timer/CR8/hardware-event gate; cleanup follows'
