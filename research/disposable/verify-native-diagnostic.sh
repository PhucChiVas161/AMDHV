#!/bin/zsh
set -euo pipefail
ENGINE=(/opt/podman/bin/podman --remote --url ssh://root@192.168.64.3/run/podman/podman.sock --identity /Users/phucduong/.local/share/containers/podman/machine/machine)
trap '"${ENGINE[@]}" rm -f fixvm-diagnostic-web >/dev/null 2>&1 || true' EXIT
print 'DIAGNOSTIC: clone GRUB helper restores ID; loaded0.3.34,1vCPU896MiB; not default-provider completion'
"${ENGINE[@]}" run --rm docker.io/library/alpine:3.22 sh -c 'uname -m; echo FIXVM_MACOS_REMOTE_CONTAINER_OK'
"${ENGINE[@]}" run --rm docker.io/library/alpine:3.22 nslookup example.com
"${ENGINE[@]}" run --rm -v /Users:/work docker.io/library/alpine:3.22 sh -c 'cat /work/fixvm-marker.txt; echo FIXVM_BIND_WRITE_OK > /work/container-output.txt'
"${ENGINE[@]}" volume create fixvm-diagnostic-data
"${ENGINE[@]}" run --rm -v fixvm-diagnostic-data:/data docker.io/library/alpine:3.22 sh -c 'echo FIXVM_VOLUME_PERSIST_OK > /data/proof'
"${ENGINE[@]}" run --rm -v fixvm-diagnostic-data:/data:ro docker.io/library/alpine:3.22 cat /data/proof
"${ENGINE[@]}" build -t localhost/fixvm-diagnostic:0.3.34 /Users/phucduong/Desktop/FixVM/research/disposable/build-context
"${ENGINE[@]}" run --rm localhost/fixvm-diagnostic:0.3.34
"${ENGINE[@]}" run -d --name fixvm-diagnostic-web -p 8080:80 docker.io/library/alpine:3.22 sh -c 'mkdir /www; echo FIXVM_PUBLISHED_PORT_OK > /www/index.html; exec httpd -f -p 80 -h /www'
for i in {1..10};do
    if curl --fail --silent --show-error --max-time 2 http://192.168.64.3:8080/;then
        print 'PASS: disposable diagnostic container/ DNS/virtiofs bind/volume/build/published guest-IP port'
        exit 0
    fi
    sleep 0.5
done
exit 1
