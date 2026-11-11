#!/bin/zsh
# Install build/AMDHV.kext into /Library/Extensions and rebuild the Auxiliary Kernel Collection.
# Takes effect after reboot. At boot the kext only reads CPUID; the VMRUN self-test runs
# only when you ask for it (scripts/selftest.sh).
# Kill switch: boot-arg -amdhvoff (AMDHV refuses to start).
set -euo pipefail
ROOT=${0:A:h:h}
DEST=/Library/Extensions/AMDHV.kext
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
[[ -d $ROOT/build/AMDHV.kext ]] || { echo "Build first: make"; exit 1; }

rm -rf /private/var/tmp/fixvm            # old staging location from the first attempt
rm -rf $DEST
cp -R $ROOT/build/AMDHV.kext $DEST
chown -R root:wheel $DEST
chmod -R go-w $DEST

AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" $DEST/Contents/Info.plist)
in_aux() { kmutil inspect -A $AUX 2>/dev/null | grep -i com.fixvm.driver.AMDHV; }

echo "--- kmutil load (stages kext, rebuilds aux collection)"
# -z skips the System Settings approval (allowed because SIP has CSR_ALLOW_UNAPPROVED_KEXTS)
kmutil load -z -p $DEST || echo "(kmutil load exit $?)"

if ! in_aux | grep -q "$VER.*$DEST"; then
  echo "--- aux collection not updated, running kmutil rebuild"
  kmutil rebuild || echo "(kmutil rebuild exit $?)"
fi

echo "--- AMDHV in aux collection:"
ls -la $AUX
if in_aux | grep -q "$VER.*$DEST"; then
  in_aux
  echo "OK: version $VER is staged. Reboot to activate."
else
  in_aux || echo "AMDHV not in aux collection"
  echo "FAILED: aux collection does not contain $VER at $DEST. Do NOT reboot, send this output."
  exit 1
fi
