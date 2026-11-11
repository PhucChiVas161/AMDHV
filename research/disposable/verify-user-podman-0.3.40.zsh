#!/bin/zsh
set -euo pipefail
ROOT=/Users/phucduong/Desktop/FixVM
P=(/opt/podman/bin/podman --connection podman-machine-default-root)
VOL=fixvm-default-validation-0340
MARK=FIXVM_DEFAULT_0340_PERSIST_OK
WEB=fixvm-default-validation-web-0340
trap '"${P[@]}" rm -f "$WEB" >/dev/null 2>&1 || true' EXIT
"${P[@]}" info --format '{{.Host.Arch}} CPUs={{.Host.CPUs}} Podman={{.Version.Version}}'
"${P[@]}" run --rm docker.io/library/alpine:3.22 sh -ec 'uname -m; nproc; nslookup example.com'
if "${P[@]}" volume exists "$VOL"; then
  [[ "$("${P[@]}" run --rm --pull=never -v "$VOL:/data:ro" docker.io/library/alpine:3.22 cat /data/proof)" == "$MARK" ]]
  print 'PASS: exact volume proof from prior VM boot'
else
  "${P[@]}" volume create "$VOL"
  "${P[@]}" run --rm -v "$VOL:/data" docker.io/library/alpine:3.22 sh -ec "printf '%s\\n' '$MARK' > /data/proof; sync"
fi
[[ "$("${P[@]}" run --rm -v "$VOL:/data:ro" docker.io/library/alpine:3.22 cat /data/proof)" == "$MARK" ]]
"${P[@]}" run --rm -v "$ROOT/research/disposable/build-context:/source:ro" docker.io/library/alpine:3.22 sh -ec 'test -s /source/Containerfile; echo FIXVM_DEFAULT_BIND_OK'
"${P[@]}" build --no-cache -t localhost/fixvm-default-validation:0.3.40 "$ROOT/research/disposable/build-context"
[[ "$("${P[@]}" run --rm localhost/fixvm-default-validation:0.3.40)" == FIXVM_BUILD_OK ]]
"${P[@]}" run --rm -i docker.io/library/alpine:3.22 sh < "$ROOT/research/disposable/cpu-stress.sh"
"${P[@]}" run -d --name "$WEB" -p 127.0.0.1:18081:80 docker.io/library/nginx:alpine
ready=0
for i in {1..20}; do
  if curl --fail --silent --max-time 2 http://127.0.0.1:18081/ >/dev/null; then ready=1; break; fi
  sleep 0.5
done
[[ "$ready" == 1 ]]
SOCKET=$(/opt/podman/bin/podman machine inspect podman-machine-default --format '{{.ConnectionInfo.PodmanSocket.Path}}')
[[ "$(curl --fail --silent --max-time 5 --unix-socket "$SOCKET" http://localhost/_ping)" == OK ]]
print 'PASS: default machine container/DNS/bind/volume/build/all-CPU hashes/published port/API'
