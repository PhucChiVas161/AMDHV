# Original Podman machine recovery — 2026-11-11

Target: `podman-machine-default`, native applehv/vfkit, original 12 CPUs,
3814 MiB RAM and 93 GiB disk. Docker was already stopped before this work.
AMDHV remains 0.3.40, fast/parallel/takeover enabled; no host reboot or kext change.

## Failure and repair

Host metadata had `Starting=true` and nonzero `LastUp`, while the guest disk still
booted with `ignition.firstboot`. Podman omitted the port-1024 Ignition vsock device
because it treated this as a subsequent boot. Linux entered emergency mode with
`failed to fetch config: dial vsock host(2):1024: connect: connection reset by peer`.
Upstream v5.8.9 `apple.StartGenericAppleVM` gates this device/server on `IsFirstBoot`;
`shim.Stop` writes LastUp even when stopping an unsuccessful boot.

Before changing metadata or provisioning, APFS-cloned the complete original disk
and copied JSON, Ignition and EFI variable store into
`research/backups/podman-default-0.3.40/`. These contain machine credentials and
must remain local. Restored LastUp to zero and Starting=false while stopped so
Podman could supply the existing original Ignition file and identity.

The first repaired boot reached the API, but its processes were terminated when
the tool's shell finished. Subsequent retry exposed an interrupted Ignition
`/etc/group` lock. Preserved that intermediate disk separately, restored the
pre-task disk clone, and performed first boot with a detached process via
Python Popen(start_new_session=True). No pristine replacement image, machine
deletion or identity replacement was used. The successful first boot then
completed and stayed running across tool invocations.

Also repaired the original Podman Desktop Ansible playbook's missing host
`~/.config/containers/registries.conf`: created a comment-only file (no registry
policy override), ran the original playbook as core, and reset the failed unit.
Ansible: ok=2, changed=1, failed=0; systemd reported zero failed units.

## Validation

`research/disposable/verify-user-podman-0.3.40.zsh` explicitly selects the original
root connection and uses separate validation objects. First full workload passed:
container, DNS, read-only bind, dedicated persistent volume, uncached image build,
strict 32 MiB SHA256 on every CPU 0–11, localhost port 18081, native API socket.
Temporary web container cleaned up. Validation volume
`fixvm-default-validation-0340` retained for exact proof after restart.

Logs: `research/runs/0.3.40-podman-user-default-*`. Early failed attempts are
diagnostics, not passing runs. Default-machine start with the normal socket alias
successfully reported API forwarding on `/var/run/docker.sock`.

Sleep/wake and long-duration stability remain untested in this task.

Second complete workload passed after normal stop/start, including exact prior
volume proof before writes and all 12 CPU hashes again. Restart cmdline no longer
has ignition.firstboot and has no tsc=reliable override. Zero failed systemd units;
no unstable/softlockup/BUG/Oops matches in collected restart dmesg. Global Docker
API alias ping also OK. Final state intentionally leaves this Podman machine
running for the user (vms1); Docker remains stopped. Two bounded workload cycles
do not establish long-duration stability or sleep/wake qualification.
