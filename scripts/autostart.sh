#!/bin/zsh
# Boot-time activation of the AMDHV backend, so kern.hv_support=1 is already
# set when Docker Desktop / Podman start after a host reboot.
#
#   sudo scripts/autostart.sh install     install the LaunchDaemon (acts from the next boot)
#   sudo scripts/autostart.sh uninstall   remove it again
#   sudo scripts/autostart.sh reset       clear the "previous activation did not finish" guard
#        scripts/autostart.sh status      show files, guard state and the last activation log
#
# The daemon is a small root-owned script outside the project tree. It only
# flips the same sysctls as desktop-backend.sh start, and only for the exact
# kext version it was installed for. It never loads, unloads or rebuilds a kext.
# Recovery if a boot ever goes wrong: boot-arg -amdhvoff (kext refuses to start,
# the daemon then finds no sysctl and exits), or safe mode.
set -euo pipefail
ROOT=${0:A:h:h}
LABEL=com.fixvm.amdhv.activate
SUPPORT='/Library/Application Support/AMDHV'
HELPER="$SUPPORT/amdhv-activate"
GUARD="$SUPPORT/activation-in-progress"
PLIST=/Library/LaunchDaemons/$LABEL.plist
LOG=/var/log/amdhv-activate.log
action=${1:-status}
[[ $# -le 1 && ( $action == install || $action == uninstall || $action == reset || $action == status ) ]] || {
  echo 'Usage: autostart.sh [install|uninstall|reset|status]'; exit 2;
}

if [[ $action == status ]]; then
  [[ -f $PLIST ]] && echo "daemon: $PLIST" || echo 'daemon: not installed'
  [[ -x $HELPER ]] && echo "helper: $HELPER (for AMDHV $(sed -n 's/^EXPECTED=//p' "$HELPER"))" || echo 'helper: not installed'
  [[ -e $GUARD ]] && echo "guard: SET - automatic activation is suspended (sudo $0 reset)" || echo 'guard: clear'
  sysctl debug.amdhv.takeover debug.amdhv.fast debug.amdhv.parallel debug.amdhv.vms kern.hv_support 2>&1 || true
  [[ -r $LOG ]] && { echo "--- $LOG (last 20 lines)"; tail -20 "$LOG"; } || true
  exit 0
fi
[[ $EUID == 0 ]] || { echo "Run with sudo: $0 $action"; exit 2; }

if [[ $action == reset ]]; then
  rm -f "$GUARD"; echo 'guard cleared'; exit 0
fi

if [[ $action == uninstall ]]; then
  launchctl bootout system/$LABEL 2>/dev/null || true
  rm -f "$PLIST" "$HELPER" "$GUARD"
  rmdir "$SUPPORT" 2>/dev/null || true
  echo 'Automatic activation removed. The running backend state is unchanged.'
  exit 0
fi

VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$ROOT/AMDHV/Info.plist")
[[ $VERSION == <->.<->.<-> ]] || { echo "Unexpected kext version '$VERSION'"; exit 1; }
mkdir -p "$SUPPORT"
chown root:wheel "$SUPPORT"; chmod 755 "$SUPPORT"
TMP=$(mktemp "$SUPPORT/.helper.XXXXXX")
cat > "$TMP" <<HELPER_EOF
#!/bin/zsh
# Installed by FixVM scripts/autostart.sh. Do not edit: reinstall instead.
EXPECTED=$VERSION
GUARD='$GUARD'
HELPER_EOF
cat >> "$TMP" <<'HELPER_EOF'
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
say() { print -r -- "$(date '+%Y-%m-%d %H:%M:%S') $*"; }
value() { sysctl -n "$1" 2>/dev/null; }

# A guard left behind means the previous automatic activation never reached its
# end (panic, hang or power loss). Do not repeat it at every boot.
if [[ -e $GUARD ]]; then
  say "guard present: previous activation did not finish; staying disabled (autostart.sh reset)"; exit 0
fi

# The auxiliary kernel collection is loaded from user space, after launchd is up.
for i in {1..120}; do value debug.amdhv.takeover >/dev/null && break; sleep 1; done
value debug.amdhv.takeover >/dev/null || { say 'AMDHV sysctls absent (kext not loaded or -amdhvoff); nothing to do'; exit 0; }

loaded=$(kmutil showloaded 2>/dev/null | sed -n 's/.*com\.fixvm\.driver\.AMDHV (\([^)]*\)).*/\1/p' | head -1)
[[ $loaded == $EXPECTED ]] || { say "loaded AMDHV '$loaded' is not the validated $EXPECTED; not activating"; exit 0; }

# AMDHV replaces AppleHV's trap registration. AppleHV must have finished its own
# start first, otherwise its failure path would release the tables again.
for i in {1..60}; do
  ioreg -r -c AppleHV -d 1 2>/dev/null | grep -q 'registered' && break; sleep 1
done
ioreg -r -c AppleHV -d 1 2>/dev/null | grep -q 'registered' || { say 'AppleHV service not registered; not activating'; exit 0; }
sleep 3

if [[ $(value debug.amdhv.takeover) == 1 ]]; then say 'backend already enabled'; exit 0; fi
[[ $(value debug.amdhv.vms) == 0 ]] || { say 'VMs present before activation; not touching'; exit 0; }

: > "$GUARD" && sync
ok=1
sysctl debug.amdhv.parallel=1 || ok=0
sysctl debug.amdhv.fast=1 || ok=0
sysctl debug.amdhv.trace_exits=0 || ok=0
(( ok )) && { sysctl debug.amdhv.takeover=1 || ok=0; }
if (( ! ok )); then
  sysctl debug.amdhv.takeover=0 >/dev/null 2>&1
  say "activation failed: $(ioreg -r -c AMDHV -l -w0 2>/dev/null | grep 'Takeover Status' | head -1)"
  rm -f "$GUARD"; exit 0
fi
rm -f "$GUARD"; sync
say "backend enabled: AMDHV $loaded fast=$(value debug.amdhv.fast) parallel=$(value debug.amdhv.parallel) kern.hv_support=$(value kern.hv_support)"
HELPER_EOF
zsh -n "$TMP"
chown root:wheel "$TMP"; chmod 755 "$TMP"; mv -f "$TMP" "$HELPER"

cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/zsh</string>
		<string>$HELPER</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>LaunchOnlyOnce</key>
	<true/>
	<key>StandardOutPath</key>
	<string>$LOG</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
PLIST_EOF
chown root:wheel "$PLIST"; chmod 644 "$PLIST"
plutil -lint "$PLIST" >/dev/null
rm -f "$GUARD"
echo "Installed for AMDHV $VERSION: $PLIST"
echo 'It acts at the next boot. Nothing was activated or changed in the running system.'
echo 'After upgrading the kext, run this install again so the validated version matches.'
