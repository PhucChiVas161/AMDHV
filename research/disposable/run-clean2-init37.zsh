#!/bin/zsh
set -euo pipefail
[[ $EUID -eq 0 ]] || exit 2
[[ $(sysctl -n debug.amdhv.takeover) == 0 && $(sysctl -n debug.amdhv.vms) == 0 ]] || exit 2
PROFILE=/Users/phucduong/Desktop/FixVM/research/disposable/podman-profile
RUN=(sudo -u "${SUDO_USER:-root}" env "TMPDIR=$(sudo -u "${SUDO_USER:-root}" getconf DARWIN_USER_TEMP_DIR)" "XDG_CONFIG_HOME=$PROFILE/config" "XDG_DATA_HOME=$PROFILE/data" CONTAINERS_MACHINE_PROVIDER=applehv)
cleanup(){ local r=$?;trap - EXIT;sysctl debug.amdhv.takeover=0 || r=1;[[ $(sysctl -n debug.amdhv.vms) == 0 ]] || r=1;exit $r; }
trap cleanup EXIT
sysctl debug.amdhv.takeover=1
"${RUN[@]}" /opt/podman/bin/podman machine init --cpus 2 --memory 4096 --disk-size 16 --rootful --image /Users/phucduong/.local/share/containers/podman/machine/applehv/cache/76ee49c13a19686e099e39255a6254636b9ac1c0b8edca356eb64eee6e3aac2c.raw.zst --volume /Users/phucduong/Desktop/FixVM/research/disposable/shares/Users:/Users fixvm-clean2
