#!/bin/zsh
set -euo pipefail
ROOT=/Users/phucduong/Desktop/FixVM
"$ROOT/research/disposable/verify-docker-reboot-0.3.38.zsh" "$1"
print 'SCOPE: full Docker workload follows; nested time measures affinity fixture including container/package overhead'
/usr/bin/time -p "$ROOT/research/disposable/verify-docker-smp4-0.3.37.zsh" "$1"
