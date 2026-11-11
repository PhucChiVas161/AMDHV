#!/bin/zsh
# Remove AMDHV and rebuild the aux collection without it. Takes effect after reboot.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
rm -rf /Library/Extensions/AMDHV.kext /private/var/tmp/fixvm
kmutil rebuild || true
kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -i fixvm || echo "AMDHV removed from aux collection"
echo "Reboot to finish."
