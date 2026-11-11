# Cache and SMP audit — 0.3.36 (2026-11-11)

## Hardware baseline: loaded 0.3.35

All three normal gates passed: `runs/0.3.35-vcpu-test.log`,
`runs/0.3.35-vm-test.log`, `runs/0.3.35-fast-test.log`. This includes the
new >4 GiB guest-memory probe and ID-flag tests in real/protected/long mode.

**Normal native Podman works with one vCPU and 2 GiB RAM.**
`runs/0.3.35-native-podman-workload-v3.log` records successful provider start,
Mac CLI/guest engine 5.8.9, Alpine execution, external DNS, virtiofs bind read/write,
persistent named volumes, image build/run, nginx at localhost:18080 and API
`/_ping = OK`, followed by normal `podman machine stop` and idle backend.
The build reused layers; this is not an uncached-build or performance result.
The old diagnostic volume also survived host reboot and normal guest boot.

The test uses `scripts/native-podman-probe.sh`, an isolated XDG profile,
`fixvm-native`, a disposable disk/EFI-vars clone and disposable shares. The real
applehv provider starts vfkit/Virtualization.framework and gvproxy; no QEMU/TCG.
GRUB is the restored original, with no diagnostic flags patch or feature-clear.
The profile was registered from copied existing configuration and provisioned disk;
a clean `machine init` has not been validated. Original Podman disk was not booted.

The provider reports an API-ping warning because the global Docker socket alias
still targets the inactive original default machine. The isolated machine's actual
API socket passes curl. Global Docker alias behavior and Docker Desktop remain
unverified. First workload logs remain: v1 selected an Alpine image without httpd;
v2 hit an existing volume fixture. The corrected v3 test is the complete PASS.

## Distinct SMP failures, not one inferred cause

- Native EFI machine with two vCPUs: `runs/0.3.35-native-podman-smp2.log`.
  AP receives #GP (SVM 0x4d), CS base 0x86000, IP 0x10000, CR0 0x30,
  CR4 0, exception bitmap zero. Root cause unresolved. BSP exit 0x41 can
  represent our consumed-instruction/budget sentinel; it does not prove TF leakage.
- Direct Alpine Linux with two vCPUs:
  `runs/0.3.35-vfkit-linux-smp2.log` and `.amdhv.log`.
  Concrete unsupported WBINVD (SVM 0x89), CS base 0x99000, IP 1, NRIP 3,
  CR0 0x30. This is the CLI;WBINVD startup sequence before console initialization.
- Hardware AP probes on loaded35 pass: `0.3.35-apreset-probe.log`,
  `0.3.35-apreset-slot1.log`, `0.3.35-aptransition-slot1.log`.
  Slot1 is owned/run by a separate pthread with relative-TSC mode, keeper slot0
  idle. Real-mode nonzero CS, segment loads, CR0 access, LGDT/LIDT, setting PE
  and far jump into flat protected mode all reach CPUID at the expected address.
  These synthetic probes do not reproduce full INIT/SIPI, concurrent BSP/AP handoff,
  interrupt controller operation or firmware timing. No SMP success claim.

## Group review and implementation

Firmware VZG11 SHA256:
`3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903`.
Fresh extraction and full 70-PE inventory:
`efi-feature-inventory-0.3.36-25G332.json`, compared against35.
Static linear decoding has mixed-mode/embedded-data false positives, excludes TE/raw
and dynamic code, and proves no runtime reachability. CpuDxe includes WBINVD at
RVA 0x10ef/0x1112/0x389f; AP prefix also reviewed as actual 16-bit bytes.
CpuDxe SHA256: `3a2eefa780dfb6f7e7ba46ee59601b7050f149521a6e1e13aa9c90eb96300694`.

Review covers CLI/STI/shadow, INVD/WBINVD/WBNOINVD, CR0 cache bits, PAT/NPT,
CLFLUSH, INVLPG/INVPCID, mode/GDT/IDT transition and user privilege behavior.
Existing CR0/PAT/NPT policy is unchanged. No INVPCID/MONITOR/MWAIT or VMX secondary
control support is added. Native hardware handles ordinary cache-line instructions;
no guest cache operation executes as host INVD/WBINVD.

0.3.36 consumes intercepted WBINVD/WBNOINVD for coherent guest RAM with no
assigned/noncoherent DMA, using MFENCE, validated hardware NRIP, retired interrupt
shadow and cleared RF. Work counts toward the existing bounded handled-exit quantum.
This model must be revisited before noncoherent DMA/device passthrough/cache-type
changes. It does not emulate physical cache invalidation on the host.
INVD is forwarded as mandatory VMX reason13 with validated exact instruction
length and unchanged RIP; the caller must emulate/advance. WBINVD-exiting secondary
control bit6 remains rejected, like other unsupported secondary controls.

Primary reference for the coherent-memory policy:
[Linux v6.12 KVM x86](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/x86.c)
(`need_emulate_wbinvd`, `kvm_emulate_wbinvd_noskip`, `kvm_emulate_invd`).
AP assembly comparison:
[EDK2 MpFuncs.nasm](https://github.com/tianocore/edk2/blob/master/UefiCpuPkg/Library/MpInitLib/X64/MpFuncs.nasm).
The local firmware bytes, not upstream source identity, determine the AP fixture.

First real-mode AP entry now logs readable first16 bytes plus full segment/control
state, once per AP lifetime, before disabling host interrupts. Failed reads are
explicitly marked unreadable. This is diagnostic evidence for the unresolved EFI
path, not an AP fix. Unsupported exits now include full state after host restoration.

## Verification and next activation

`runs/0.3.36-offline-checks.log`: build/sign/imports339, ASan/UBSan units,
FP/XSAVE switches, resolver6 tests and all script syntax PASS, no compiler warning.
Compiled `vcpu_svm.o` disassembly saved in
`runs/0.3.36-vcpu-svm-disassembly.txt`: MFENCE present; no host INVD/WBINVD/WBNOINVD.
New signed cacheprobe covers16/32/64, 64 alternating cache instructions across
quantum boundaries, guest stores/flags/registers, INVD prefix length/unconsumed RIP
and resume. CPL3 probe includes INVD/WBINVD #GP. AP reset fixture now starts with
CLI;WBINVD. Hardware for these new cases is pending loaded36.

After reboot: vCPU --modes → VM --rom → fast-test; only after all pass,
repeat one-vCPU native Podman, then direct Linux SMP2 and native EFI SMP2.
Inspect new first-AP bytes/state before changing AP execution/scheduling policy.
SMP, clean initialization, Docker Desktop, full lifecycle and performance acceptance
remain open. Source36/offline PASS does not mean hardware36 or the goal is complete.

Installation complete: AuxKC36, loaded35, backup35 retained. All four idle gates zero; stopped isolated CPU1/2GiB profile. Original disk inode/size/mtime unchanged. Hardware36 pending manual reboot. See `runs/0.3.36-install.log`.

Follow-up on loaded36: first-AP fetch was always unreadable because it requested16 bytes from a helper capped at15; no memory fault can be inferred from those unreadable records. Fixed in37. Both SMP trials36 reached Linux AP EFER901 preparation; the old EFI #GP was not reproduced, but its root cause is not established.
