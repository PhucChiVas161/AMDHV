#!/bin/zsh
# Run the VMRUN self-test on every CPU (needs root), then show results.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
sync                                     # flush disks in case something goes wrong
sysctl debug.amdhv.selftest=1
${0:A:h}/status.sh
