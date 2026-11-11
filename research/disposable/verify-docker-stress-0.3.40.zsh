#!/bin/zsh
set -euo pipefail
ROOT=/Users/phucduong/Desktop/FixVM
"$ROOT/research/disposable/verify-docker-0.3.39.zsh" "$1"
print 'SCOPE: four-container cached-image CPU/storage stress; time includes container/API overhead'
/usr/bin/time -p "$ROOT/research/disposable/docker-stress-0.3.40.zsh" "$1"
