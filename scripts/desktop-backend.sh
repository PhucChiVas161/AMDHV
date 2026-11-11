#!/bin/zsh
# Manual backend activation (fast + parallel). Requires the loaded kext to be 0.3.42.
# Does not launch a VM, change boot settings or unload the kext.
set -euo pipefail
action=${1:-status}
[[ $# -le 1 && ( $action == start || $action == stop || $action == status ) ]] || {
 echo 'Usage: desktop-backend.sh [start|stop|status]';exit 2;
}
status(){
 kmutil showloaded 2>/dev/null | grep 'com.fixvm.driver.AMDHV' || true
 sysctl debug.amdhv.fast debug.amdhv.parallel debug.amdhv.trace_exits debug.amdhv.takeover debug.amdhv.vms kern.hv_support
}
if [[ $action == status ]];then status;exit;fi
[[ $EUID == 0 ]] || { echo "Run with sudo: $0 $action";exit 2; }
if [[ $action == start ]];then
 kmutil showloaded 2>/dev/null | grep -q 'com.fixvm.driver.AMDHV (0.3.42)' || {
  echo 'Requires loaded AMDHV 0.3.42 (install it and reboot first)';exit 1;
 }
 if [[ $(sysctl -n debug.amdhv.takeover) == 1 ]];then
  [[ $(sysctl -n debug.amdhv.fast) == 1 && $(sysctl -n debug.amdhv.parallel) == 1 ]] || {
   echo 'Backend already active with a different configuration; stop all VMs before changing it';exit 1;
  }
  echo 'Backend already enabled';status;exit;
 fi
 [[ $(sysctl -n debug.amdhv.vms) == 0 ]] || { echo 'Stop all VMs first';exit 1; }
 saved_fast=$(sysctl -n debug.amdhv.fast)
 saved_parallel=$(sysctl -n debug.amdhv.parallel)
 saved_trace=$(sysctl -n debug.amdhv.trace_exits)
 rollback(){
  local result=$?;trap - EXIT
  if (( result )) && [[ $(sysctl -n debug.amdhv.vms) == 0 ]];then
   sysctl debug.amdhv.takeover=0 || true
   sysctl debug.amdhv.fast=$saved_fast || true
   sysctl debug.amdhv.parallel=$saved_parallel || true
   sysctl debug.amdhv.trace_exits=$saved_trace || true
  fi
  exit $result
 }
 trap rollback EXIT
 sysctl debug.amdhv.parallel=1
 sysctl debug.amdhv.fast=1
 sysctl debug.amdhv.trace_exits=0
 sysctl debug.amdhv.takeover=1
 echo 'Backend enabled; Docker Desktop can now start its Apple VZ VM (tested 4 CPU / 4 GiB)'
else
 [[ $(sysctl -n debug.amdhv.vms) == 0 ]] || {
  echo 'Quit Docker Desktop and stop Podman machines before disabling the backend';exit 1;
 }
 sysctl debug.amdhv.takeover=0
 sysctl debug.amdhv.fast=0
 sysctl debug.amdhv.parallel=0
 sysctl debug.amdhv.trace_exits=0
 echo 'Backend disabled'
fi
status
