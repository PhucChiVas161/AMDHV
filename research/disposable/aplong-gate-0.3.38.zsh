#!/bin/zsh
set -euo pipefail
[[ $EUID == 0 && $(sysctl -n debug.amdhv.vms) == 0 && $(sysctl -n debug.amdhv.takeover) == 0 ]] || exit 2
saved_fast=$(sysctl -n debug.amdhv.fast)
cleanup(){
 local result=$?;trap - EXIT
 sysctl debug.amdhv.takeover=0 || result=1
 sysctl debug.amdhv.fast=$saved_fast || result=1
 sysctl -n debug.amdhv.log
 [[ $(sysctl -n debug.amdhv.vms) == 0 ]] || result=1
 exit $result
}
trap cleanup EXIT
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.takeover=1
sudo -u "$SUDO_USER" /Users/phucduong/Desktop/FixVM/build/aplongprobe
