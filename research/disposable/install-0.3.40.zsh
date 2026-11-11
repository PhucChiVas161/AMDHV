#!/bin/zsh
set -euo pipefail
cd /Users/phucduong/Desktop/FixVM
[[ $EUID == 0 ]] || exit 2
for key in debug.amdhv.vms debug.amdhv.takeover debug.amdhv.fast kern.hv_support;do
 [[ $(sysctl -n "$key") == 0 ]] || { print "Not idle: $key";exit 2; }
done
[[ $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' /Library/Extensions/AMDHV.kext/Contents/Info.plist) == 0.3.39 ]] || exit 2
[[ $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' build/AMDHV.kext/Contents/Info.plist) == 0.3.40 ]] || exit 2
codesign --verify --deep --strict build/AMDHV.kext
backup=research/kext-backups/AMDHV-0.3.39.kext
[[ ! -e $backup ]] || { print "Backup already exists; inspect before replacing";exit 2; }
cp -Rp /Library/Extensions/AMDHV.kext "$backup"
codesign --verify --deep --strict "$backup"
scripts/install.sh
codesign --verify --deep --strict /Library/Extensions/AMDHV.kext
kmutil showloaded 2>/dev/null | grep 'com.fixvm.driver.AMDHV'
