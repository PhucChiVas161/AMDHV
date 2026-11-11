#!/bin/zsh
set -euo pipefail
SOCKET=$1
D=(/Applications/Docker.app/Contents/Resources/bin/docker -H "unix://$SOCKET")
trap '"${D[@]}" rm -f fixvm-docker-web >/dev/null 2>&1 || true' EXIT
"${D[@]}" version
"${D[@]}" run --rm alpine:3.22 sh -c 'uname -a; nproc; echo FIXVM_DOCKER_NATIVE_OK; nslookup example.com'
"${D[@]}" run --rm -v /Users/phucduong/Desktop/FixVM/research/disposable/shares/Users:/work alpine:3.22 sh -c 'cat /work/fixvm-marker.txt; echo FIXVM_DOCKER_BIND_OK > /work/docker-output.txt'
if "${D[@]}" volume inspect fixvm-docker-data >/dev/null 2>&1;then
 "${D[@]}" run --rm -v fixvm-docker-data:/data:ro alpine:3.22 cat /data/proof
else
 "${D[@]}" volume create fixvm-docker-data
fi
"${D[@]}" run --rm -v fixvm-docker-data:/data alpine:3.22 sh -c 'echo FIXVM_DOCKER_PERSIST_OK > /data/proof'
"${D[@]}" run --rm -v fixvm-docker-data:/data:ro alpine:3.22 cat /data/proof
"${D[@]}" build --no-cache -f /Users/phucduong/Desktop/FixVM/research/disposable/build-context/Containerfile -t fixvm-docker:0.3.37 /Users/phucduong/Desktop/FixVM/research/disposable/build-context
"${D[@]}" run --rm fixvm-docker:0.3.37
/usr/bin/time -p "${D[@]}" run --rm -i alpine:3.22 sh < /Users/phucduong/Desktop/FixVM/research/disposable/cpu-stress.sh
"${D[@]}" run -d --name fixvm-docker-web -p 127.0.0.1:18081:80 nginx:alpine
for i in {1..20};do
 if curl --fail --silent --show-error --max-time 2 http://127.0.0.1:18081/;then
  curl --fail --silent --show-error --max-time 2 --unix-socket "$SOCKET" http://localhost/_ping
  print '\nPASS: Docker native container/DNS/bind/volume/uncached-build/SMP/localhost port'
  exit 0
 fi
 sleep 0.5
done
exit 1
