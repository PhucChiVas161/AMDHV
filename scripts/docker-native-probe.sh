#!/bin/zsh
# Native Docker Desktop backend probe, no Podman/QEMU/socket alias fallback.
set -euo pipefail
ROOT=${0:A:h:h}
[[ $EUID -eq 0 ]] || { echo 'Run with sudo';exit 2; }
# Headless engine tests otherwise permit macOS idle sleep mid-workload. Tie the
# temporary assertion to this probe's lifetime, including teardown.
/usr/bin/caffeinate -i -w $$ &
print "Idle-sleep guard PID=$! (until probe exits)"
PARALLEL=${FIXVM_PARALLEL:-0}
[[ $PARALLEL == 0 || $PARALLEL == 1 ]] || { echo 'FIXVM_PARALLEL must be 0 or 1';exit 2; }
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || { echo 'Backend must be idle';exit 2; }
backend_pattern='^/Applications/Docker[.]app/Contents/MacOS/com[.]docker[.]backend( |$)'
if pgrep -f "$backend_pattern" >/dev/null;then
 echo 'Docker backend processes must finish shutting down before this probe';exit 2;
fi
RUN=(sudo -u "${SUDO_USER:-root}" env "PATH=/Applications/Docker.app/Contents/Resources/bin:$PATH" "TMPDIR=$(sudo -u "${SUDO_USER:-root}" getconf DARWIN_USER_TEMP_DIR)")
DOCKER=/Applications/Docker.app/Contents/Resources/bin/docker
SOCKET=/Users/phucduong/.docker/run/docker.sock
SAVED_FAST=$(sysctl -n debug.amdhv.fast)
SAVED_SUPPORT=$(sysctl -n kern.hv_support)
SAVED_PARALLEL=$(sysctl -n debug.amdhv.parallel)
launcher=0
cleanup(){
 local r=$?;trap - EXIT
 "${RUN[@]}" "$DOCKER" desktop stop --timeout 20 || true
 if (( launcher ));then kill -TERM $launcher 2>/dev/null || true;fi
 # VM count can reach zero before the backend releases its single-instance
 # socket. Wait for monitor/services too before another start is allowed.
 for i in {1..150};do pgrep -f "$backend_pattern" >/dev/null || break;sleep 0.1;done
 if pgrep -f "$backend_pattern" >/dev/null;then
  echo 'FAIL: Docker backend processes did not finish shutting down';r=1
 else
  echo 'PASS: Docker backend process shutdown completed'
 fi
 if (( launcher )) && ! kill -0 $launcher 2>/dev/null;then wait $launcher 2>/dev/null || true;fi
 for i in {1..200};do [[ $(sysctl -n debug.amdhv.vms) == 0 ]] && break;sleep 0.1;done
 sysctl debug.amdhv.takeover=0 || r=1
 sysctl debug.amdhv.fast=$SAVED_FAST || r=1
 sysctl debug.amdhv.parallel=$SAVED_PARALLEL || r=1
 sysctl -n debug.amdhv.log
 [[ $(sysctl -n debug.amdhv.vms) == 0 && $(sysctl -n kern.hv_support) == $SAVED_SUPPORT ]] || r=1
 exit $r
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sysctl debug.amdhv.log=clear >/dev/null
sysctl debug.amdhv.parallel=$PARALLEL
sysctl debug.amdhv.fast=1
sysctl debug.amdhv.takeover=1
"${RUN[@]}" /Applications/Docker.app/Contents/MacOS/com.docker.backend -accept-license -with-frontend=false -unattended &
launcher=$!
ready=0
for i in {1..240};do
 if "${RUN[@]}" curl --silent --max-time 1 --unix-socket "$SOCKET" http://localhost/_ping 2>/dev/null | /usr/bin/grep -qx OK;then ready=1;break;fi
 sleep 0.5
done
(( ready )) || { echo 'FAIL: Docker engine did not become ready';exit 1; }
"${RUN[@]}" "$DOCKER" -H "unix://$SOCKET" info
if [[ -n ${1:-} ]];then "${RUN[@]}" "$1" "$SOCKET";fi
print 'PASS: native Docker engine ready; cleanup follows'
