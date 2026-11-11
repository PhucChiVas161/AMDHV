# 0.3.35: EFI CPUID discovery and bounded multi-GiB guest memory

2026-11-11. Loaded baseline0.3.34; new0.3.35 kernel behavior pending reboot.
The full native Podman/Docker goal remains incomplete. This turn produced the
first successful diagnostic containers through macOS Podman and native vfkit.

## Normal runtime evidence on0.3.34

Normal vCPU --modes630 / VM --rom29 / fast-test203 PASS lines, zero FAIL:
`runs/0.3.34-{vcpu-test,vm-test,fast-test}.log`. New speculation tests prove
accepted guest SPEC_CTRL readback/re-entry/reset, command execution, reserved
bit/read-only faults, bitmap routing, prefixes/NRIP and host control equality
checks in the kernel. Not a side-channel effectiveness certification.

Default Alpine, no feature-clearing/mitigations=off, reaches Alpine Init and
its emergency shell because this diskless probe has no boot media:
`runs/0.3.34-vz-linux10.log`. No Oops/panic. Through the actual bundled vfkit,
interactive guest commands run uname/id,25 separate busybox executions,
write/hash1MiB (SHA25630e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58),
then guest poweroff succeeds. `runs/0.3.34-vfkit-linux-shell.log` and adjacent
`.amdhv.log`. This proves real guest userspace execution and clean shutdown,
not a normal installed rootfs or the complete provider/container lifecycle.

## Disposable EFI path and grouped findings

APFS `cp -c` makes `disposable/podman-efi-test.raw` from the stopped original
93GiB Podman raw image. Separate EFI vars under `disposable/` are used. Source
image inode165146, size99857989632, mtime1793953935 is unchanged. Original
Podman machine is not started/rewritten. Ignition is read from the original
configuration; host shares for the test contain only disposable directories.

- EFI2GiB start fails mapping80000000 bytes with HV_NO_RESOURCES, before vCPU.
  `runs/0.3.34-vfkit-podman-efi.log.amdhv.log`: existing per-VM pin cap1GiB.
- EFI512MiB with original GRUB gets to CPUID1cd54e35 then VMM advances to
  1cd54e37. RFLAGS200202, otherwise accepted ring0 code64/TSS shape; pending
  IRQ40. The validator rejects IDbit21. `0.3.34-vfkit-podman-efi512.log.amdhv.log`.
- Audited related capacity constraints: old NPT1024 nodes cannot densely back
  a realistic4GiB VM either. Increasing only the pin cap would still fail.
- Expanded pin budget requires aggregate accounting, complete-descriptor
  lifetime across partial unmaps, rollback, VM/task teardown, sparse NPT bounds
  and metadata bounds. All paths remain serialized by the existing backend gate.

GRUB was extracted by mounting ONLY the clone via hdiutil, read-only initially.
GRUB SHA256db283a408682e92dabec2c2098576c2a6e374e714320124a0161136c5b326095.
Its CPUID RVA10e35 matches the observed runtime address with inferred base1cd44000.
Helper RVA10da9 does PUSHFQ/XOR ID/POPFQ and deliberately leaves ID toggled.
`runs/0.3.35-grub-id-disassembly.txt` and `efi-loader-cpuid-0.3.35.json`.
The address-to-module association is an inference corroborated by the helper,
not an independently observed firmware loader map.

Full firmware70-PE inventory rerun is identical to0.3.32:
`efi-feature-inventory-0.3.35-25G332.json`; unchanged VZG11 SHA256
3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903.
Additional BOOTX64/shim and GRUB executable-section scan:
`efi-loader-inventory-0.3.35.json`. Static linear decoding has embedded-data,
mixed-mode and reachability limitations; TE/raw/dynamic code is excluded.

## Diagnostic-only rootfs and container success

To inspect later failures BEFORE requesting another reboot, the clone GRUB
helper temporarily restored incoming ID on return. This did not alter the
loaded kext or normal hardware gate. Exact patch bytes/disassembly/hashes:
`runs/0.3.34-grub-id-restore-diagnostic.txt`. This guest repair is NOT a default
boot proof or an acceptable final dependency. Original GRUB was subsequently
restored and hash matched: `runs/0.3.34-grub-diagnostic-restored.log`.

512MiB diagnostic reaches Fedora kernel7.2.7 but fails initramfs unpacking,
then init is missing libsystemd-core and panics.896MiB unpacks successfully,
switches root and enters Fedora CoreOS44.20260928.3.2. Initial missing matching
virtiofs tags/ready listener send it into emergency mode; this is fixture
configuration. Those were provided using ONLY three disposable host directories
and a local vsock1025 Unix listener. NAT and RNG devices were added.

`runs/0.3.34-vfkit-diag-native-rootfs.log`: reaches multi-user.target, ready.service
success and Ready over vsock. SSH reports system state running. Native backend
is /opt/podman/bin/vfkit -> Apple Virtualization.framework XPC, no QEMU/TCG.
1vCPU,896MiB, loaded0.3.34, patched clone GRUB only. Both Podman client and
server5.8.9: `runs/0.3.34-macos-podman-diag-version.json`.

| Diagnostic check | Actual result |
|---|---|
| Engine architecture/CPU/storage | amd64,1CPU,overlay; guest SSH and macOS Podman remote API succeed |
| First container | alpine3.22 prints x86_64/FIXVM_CONTAINER_OK from guest; macOS Podman CLI independently prints FIXVM_MACOS_REMOTE_CONTAINER_OK |
| DNS inside container | nslookup example.com succeeds via NAT |
| virtiofs bind mount | default container bind reads host marker and writes container-output.txt visible in disposable host share |
| Named volume | separate containers write/read FIXVM_VOLUME_PERSIST_OK; volume retained for next normal boot persistence test |
| Build | macOS client builds localhost/fixvm-diagnostic:0.3.34 from a tiny isolated context; running result prints FIXVM_BUILD_OK |
| Published port | FAIL: machine-mode engine requests gateway.containers.internal/services/forwarder/expose; this NAT fixture has no gvproxy gateway DNS/service. No published-port success is claimed |
| Poweroff/cleanup | SSH systemctl poweroff reaches poweroff.target; vfkit exits normally; liveVMs/takeover/fast/hv_support all return0 |

Evidence `runs/0.3.34-{vfkit-diag-ssh,vfkit-diag-container,macos-podman-diag-info,
macos-podman-diag-workload,vfkit-diag-poweroff}.log`. Workload script returns126
at the published-port failure; earlier results are individual successes, not an
overall PASS. Web fixture removed; no containers remain. Named volume retained
at /var/lib/containers/storage/volumes/fixvm-diagnostic-data/_data in the clone.

Mac CLI used an explicit SSH URL to the disposable VM's root Podman socket.
This does NOT prove normal `podman machine start`, gvproxy/localhost ports,
SMP, host-reboot persistence, performance or Docker Desktop. Docker remains
unverified. The extra diagnostic configuration is not the final supported path.

## 0.3.35 implementation and tests

- Admit architecturally harmless RFLAGS.ID in shared PilotFlags, including
  step/legacy/fast paths and software IRET validation. Preserve its actual value;
  do not clear it to work around GRUB. Guest TF/IOPL/NT/VM/VIF/VIP/reserved flags
  remain rejected. RF/AC retain their existing native64-only policy.
- Per-VM prepared pin budget8GiB; aggregate guest backing budget16GiB under the
  backend gate. Entire descriptors remain charged until actually completed;
  partial unmaps do not pretend to return pins. Charge only successful commit;
  full unmap, release, task exit and failure rollback preserve accounting.
- NPT node ceiling8192 supports dense8GiB plus table overhead/sparse firmware.
  Each node uses a wired4KiB table plus child metadata (~64MiB at the ceiling).
 16VM ceiling remains; pin budget excludes NPT/shared/vCPU metadata. An8GiB RAM
  VM plus firmware exceeds the8GiB pin cap: this is a backing budget, not an
  advertised maximum RAM configuration. No lazy pins or huge NPT pages added.
- Unit tests execute actual GuestMemory/NPT with stub descriptors: two8GiB maps,
  aggregate/per-VM rejection, retained charge after partial unmap, release/reuse
  and failed-map rollback. These do not physically wire16GiB on the host.
- Mode tests admit ID and continue rejecting other privilege/virtual/debug bits.
  New hardware tests in flagsprobe toggle ID using proper32-bit operands in
  16-bit mode and native32/64 widths, re-entry and TF hiding. Fastprobe adds
  native64 PUSHF/POPF/CPUID with retained ID. New signed memoryprobe maps
  >4GiB across GPA4GiB..8GiB, protects/unmaps the tail and checks cleanup/reuse.
- `runs/0.3.35-offline-checks.log`: complete build/imports339, ASan/UBSan actual
  memory/mode/MSR/etc units, four FP-switch variants, resolver6 and script
  syntax PASS, no warnings. New0.3.35 hardware behavior remains pending reboot.

After reboot: vCPU --modes -> VM --rom (memoryprobe included) -> fast-test.
Only after all PASS rerun unmodified GRUB clone at2GiB/1CPU, confirm normal
Fedora startup and retained diagnostic volume. Then finish actual applehv/
gvproxy integration, guest SMP and the broader Podman/Docker acceptance criteria.

Reference for vfkit options and APFS clones (official v0.6.4 documentation):
https://github.com/crc-org/vfkit/blob/v0.6.4/doc/usage.md

Staging confirmed: install exit0, kmutil exit28 requests reboot; AuxKC contains
0.3.35 at /Library/Extensions/AMDHV.kext. Backup0.3.34 retained. Loaded remains
0.3.34; takeover/vms/fast/hv_support all0. No VM or attached clone disk remains.
Original clone GRUB restored and source image metadata unchanged. Stop at reboot.
