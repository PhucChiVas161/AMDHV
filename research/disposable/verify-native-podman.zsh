#!/bin/zsh
set -euo pipefail
NAME=$1
ENGINE=(/opt/podman/bin/podman --connection "$NAME-root")
trap '"${ENGINE[@]}" rm -f fixvm-native-web >/dev/null 2>&1 || true' EXIT
print 'NORMAL: loaded0.3.35, original GRUB, applehv provider start/stop, gvproxy; isolated profile/clone'
"${ENGINE[@]}" version --format json
"${ENGINE[@]}" run --rm docker.io/library/alpine:3.22 sh -c 'uname -m; echo FIXVM_NATIVE_CONTAINER_OK'
"${ENGINE[@]}" run --rm docker.io/library/alpine:3.22 nslookup example.com
"${ENGINE[@]}" run --rm -v /Users:/work docker.io/library/alpine:3.22 sh -c 'cat /work/fixvm-marker.txt; echo FIXVM_NATIVE_BIND_OK > /work/native-output.txt'
"${ENGINE[@]}" run --rm -v fixvm-diagnostic-data:/data:ro docker.io/library/alpine:3.22 cat /data/proof
if "${ENGINE[@]}" volume exists fixvm-native-data;then
    "${ENGINE[@]}" run --rm -v fixvm-native-data:/data:ro docker.io/library/alpine:3.22 cat /data/proof
else
    "${ENGINE[@]}" volume create fixvm-native-data
fi
"${ENGINE[@]}" run --rm -v fixvm-native-data:/data docker.io/library/alpine:3.22 sh -c 'echo FIXVM_NATIVE_PERSIST_OK > /data/proof'
"${ENGINE[@]}" run --rm -v fixvm-native-data:/data:ro docker.io/library/alpine:3.22 cat /data/proof
"${ENGINE[@]}" build -t localhost/fixvm-native:0.3.35 /Users/phucduong/Desktop/FixVM/research/disposable/build-context
"${ENGINE[@]}" run --rm localhost/fixvm-native:0.3.35
"${ENGINE[@]}" run -d --name fixvm-native-web -p 127.0.0.1:18080:80 docker.io/library/nginx:alpine
for i in {1..10};do
    if curl --fail --silent --show-error --max-time 2 http://127.0.0.1:18080/;then
        SOCKET=$(/opt/podman/bin/podman machine inspect "$NAME" --format '{{.ConnectionInfo.PodmanSocket.Path}}')
        curl --fail --silent --show-error --max-time 5 --unix-socket "$SOCKET" http://localhost/_ping
        print
        print 'PASS: native provider container/DNS/bind/volume/build/localhost port'
        exit 0
    fi
    sleep 0.5
done
exit 1
