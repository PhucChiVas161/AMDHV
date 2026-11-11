#!/bin/zsh
set -euo pipefail
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || exit 2
cleanup(){ local r=$?;trap - EXIT;sysctl debug.amdhv.takeover=0;sysctl debug.amdhv.fast=0;sysctl -n debug.amdhv.log;exit $r; }
trap cleanup EXIT
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.takeover=1
sudo -u "${SUDO_USER:-root}" /Users/phucduong/Desktop/FixVM/build/apresetprobe
