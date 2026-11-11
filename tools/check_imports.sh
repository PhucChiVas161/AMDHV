#!/bin/zsh
# Verify that every symbol the kext imports is exported by one of the KPIs it declares.
# usage: check_imports.sh <kext binary> <KDK path>
set -eu
BIN=$1; KDK=$2
PLUG=$KDK/System/Library/Extensions/System.kext/PlugIns
TMP=$(mktemp -d)
trap 'rm -rf $TMP' EXIT
for k in BSDKernel IOKit Libkern Mach Unsupported; do
  nm -g $PLUG/$k.kext/$k 2>/dev/null | sed -E 's/ \(indirect for .*\)$//' | awk '{print $NF}'
done | sort -u > $TMP/exports
nm -u $BIN | sort -u > $TMP/imports
comm -23 $TMP/imports $TMP/exports > $TMP/missing
if [[ -s $TMP/missing ]]; then
  sed 's/^/MISSING /' $TMP/missing
  exit 1
fi
echo "imports OK ($(wc -l < $TMP/imports | tr -d ' ') symbols)"
