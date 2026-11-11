#!/bin/zsh
# Phase 2: take over hv traps, verify the framework reaches our vm_create stub.
# Cleanup removes AMDHV traps and restores hv_support; reboot restores AppleHV traps.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
[[ -x $ROOT/build/hvprobe ]] || { echo "Build first: make"; exit 1; }
RUNAS=${SUDO_USER:-root}
[[ $(sysctl -n debug.amdhv.takeover) == 0 ]] || { echo "FAIL: takeover already active"; exit 1; }
SAVED_SUPPORT=$(sysctl -n kern.hv_support)

cleanup() { sysctl -q debug.amdhv.takeover=0 >/dev/null; }
on_exit() {
  local result=$?
  trap - EXIT
  if ! cleanup; then
    echo "FAIL: cleanup failed" >&2
    result=1
  fi
  exit $result
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

sync
sysctl debug.amdhv.log=clear >/dev/null
echo "--- takeover"
sysctl debug.amdhv.takeover=1 || { echo "takeover failed:"; sysctl -n debug.amdhv.log; exit 1; }
echo "kern.hv_support = $(sysctl -n kern.hv_support)"

echo "--- hvprobe (as $RUNAS)"
probe_result=0
sudo -u "$RUNAS" "$ROOT/build/hvprobe" --expect-stubs || probe_result=$?

echo "--- restore"
cleanup
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n kern.hv_support) == $SAVED_SUPPORT ]] || {
  echo "FAIL: state not restored"; exit 1;
}
echo "kern.hv_support = $(sysctl -n kern.hv_support)"

echo "--- AMDHV log"
trace_log=$(sysctl -n debug.amdhv.log)
print -r -- "$trace_log"
[[ $probe_result == 0 ]] || { echo "FAIL: hvprobe exited $probe_result"; exit 1; }
[[ $trace_log == *'task[0] vm_create'* && $trace_log == *'(hvprobe)'* ]] || {
  echo "FAIL: vm_create trace missing"; exit 1;
}
echo "PASS: phase 2 takeover, vm_create trace, and cleanup"
