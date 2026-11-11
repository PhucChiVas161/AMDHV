#!/bin/zsh
# Read existing proof before any workload can overwrite it.
set -euo pipefail
case ${1:-} in
 podman)
  [[ $# -eq 2 ]] || exit 2
  ENGINE=(/opt/podman/bin/podman --connection "$2-root")
  VOLUME=fixvm-native-data; EXPECTED=FIXVM_NATIVE_PERSIST_OK ;;
 docker)
  [[ $# -eq 2 ]] || exit 2
  ENGINE=(/Applications/Docker.app/Contents/Resources/bin/docker -H "unix://$2")
  VOLUME=fixvm-docker-data; EXPECTED=FIXVM_DOCKER_PERSIST_OK ;;
 *) print 'Usage: verify-reboot-volume-0.3.38.zsh podman MACHINE | docker SOCKET';exit 2 ;;
esac
"${ENGINE[@]}" volume inspect "$VOLUME" >/dev/null
PROOF=$("${ENGINE[@]}" run --rm --pull=never -v "$VOLUME:/data:ro" docker.io/library/alpine:3.22 cat /data/proof)
[[ $PROOF == $EXPECTED ]] || { print "FAIL: persisted proof mismatch for $VOLUME";exit 1; }
print "PASS: existing volume $VOLUME contains $EXPECTED before any write"
