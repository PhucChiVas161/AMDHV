# AP long-mode preparation audit — 0.3.37

Date: 2026-11-11. Hardware baseline: loaded AMDHV 0.3.36, macOS25G332.

## Verified results on36

Normal gates all exit0: vCPU --modes669 PASS lines, VM --rom37,
fast-test240; zero FAIL lines. Logs `runs/0.3.36-{vcpu-test,vm-test,fast-test}.log`.
Cache stream16/32/64, prefixed INVD VMX13, CPL3 cache #GP, slot1 CLI;WBINVD and
actual CpuDxe real-to-protected-mode prefix all pass on hardware.

`runs/0.3.36-native-podman-workload.log` exits0: actual applehv provider
start/info/stop, CLI/engine5.8.9, 1vCPU/2GiB, original restored GRUB,
container/DNS/virtiofs bind read-write, named-volume persistence across host reboot,
**uncached** build and run, nginx at localhost18080, actual isolated API ping OK.
No `Using cache` in this build. Same isolated profile and disposable disk/EFI/shares
as35. Normal global Docker socket alias warning remains; this does not validate
that alias or Docker Desktop. No clean machine-init or performance acceptance yet.

Both direct Alpine SMP2 and the real Podman EFI SMP2 provider now reach a
Linux AP trampoline, then stop at the **same validated-state rejection**:

```
CR0=50033, CR3=9c000, CR4=20b0 (Alpine) /20f0 (CoreOS)
EFER=901, RIP=9a05d, CS=8/base0/ARc09b, RFLAGS=200097
CR0 mask=e0000020, CR4 mask=2000, CR4 shadow=0
```

Logs: `runs/0.3.36-vfkit-linux-smp2.log` and `.amdhv.log`,
`runs/0.3.36-native-podman-smp2.log`. These runs FAIL (exit1 /125), not SMP passes.
Compared with35, direct Linux has passed WBINVD; native EFI has progressed beyond
firmware/GRUB into Linux AP startup. The old EFI #GP was not reproduced in this
run. Its root cause remains unproven; no claim that a timing/firmware defect is fixed.

## Actual bytes and grouped review

Exact unchanged Alpine bzImage SHA256:
`8ce908a8cb28d55adcacacf8a16609fa400e07a8826f0084ae5b13b8cb91b9ab`.
Fresh decompressed ELF SHA256:
`13279833ca9b4de6786d32cedb89794af7745f68304622d74e72cd941e8d4942`.
`linux/alpine-ap-trampoline-audit-0.3.37.txt` decodes its actual16/32/64-bit
trampoline from ELF file offset2316000, including the AP lock/stack, CPU checks,
GDT/IDT, PE, CR4/PAE and CR3, EFER RDMSR/WRMSR, CR0.PG, compatibility far jump
and indirect64-bit kernel entry. The in-memory relocation differs from static
operands. Runtime RIP9a05d matches startup32 offset105d immediately after
WRMSR at105b: next MOV EAX,80050033; MOV CR0,EAX; far jump64.

Reference for the sequence:
[Linux v6.12 trampoline_64.S](https://github.com/torvalds/linux/blob/v6.12/arch/x86/realmode/rm/trampoline_64.S).
SCE is separate from LME/LMA; copying EFER with SCE before enabling paging is a
valid prepared state, not active long mode. Architecture reference:
[AMD APM Volume2](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2).
The backend executes AMD guest instructions; this change does not claim complete
legacy SYSCALL/SYSRET applications or Intel-specific legacy instruction behavior.

Fresh firmware extraction/full inventory37 is identical to36 across70 PE modules:
`efi-feature-inventory-0.3.37-25G332.json`. VZG11 SHA256 remains
`3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903`.
Reviewed grouped CpuDxe AP segment/descriptor/control transitions and cache behavior
against the synthetic probes and real Linux path, not only the rejected EFER bit.
Static inventory still excludes TE/raw/dynamic code and cannot establish reachability.

## Bounded change37

`validPilotMode` permits SCE in the already-validated **LME-prepared ring0** state
only in the fast host-interrupt runner, in addition to existing active IA-32e.
Prepared state still requires PE and PAE, accepted CR0/CR4, segments, flags, page
root and controls. Bare legacy/real SCE, unsupported privilege caches, reserved EFER,
SVME, inconsistent LMA and step-runner SCE remain rejected. No guest EFER is written
to the host; VMCB owns it. No TF budget, fault-vector policy, xstate capacity, CR
mask/VMX exit ABI, scheduling or locking changes. EFER MSR remains caller-forwarded.

`longtransitionprobe --sce` exercises EFER901 before PG, masked CR0.PG exit,
caller publication of d01/IA-32e entry, compatibility-to64 far jump, full-width
store, TSS/LDTR retention, HLT/resume and NPT page-walk fault. Added to fast gate.
ASan mode tests cover prepared SCE vs step, missing PAE/PE, ring0 restriction and
existing invalid EFER/controls. Hardware37 for the extended case is pending reboot.

The new probe was also run against loaded36 to reproduce the rejection:
`runs/0.3.36-sce-preparation-negative.log`, intentional exit1. First entry rejects;
LLDT/LTR/CR0-operand assertions consequently fail. After explicit caller publication
of PG/LMA, the same probe's compatibility far jump,64-bit marker, HLT and NPT fault
pass on36. This narrows the failure but is not a whole-probe PASS or hardware37 proof.

A second grouped fix corrects AP diagnostics:36 requested16 bytes from
`fetchGuestCode`, whose bound is15, so all first-AP records said unreadable regardless
of backing.37 requests15 and explicitly labels that length (last printed byte is
zero padding). The old unreadable records prove no guest-memory mapping problem.
Diagnostic reads occur before host interrupts are disabled, once per AP lifetime.

## Checks and next activation

`runs/0.3.37-offline-checks.log` PASS: build/sign/imports339, ASan/UBSan units,
four FP/XSAVE switch variants, resolver6 and all script syntax; no compiler warning.
Compiled kernel disassembly in `runs/0.3.37-vcpu-svm-disassembly.txt` retains MFENCE
with no host INVD/WBINVD/WBNOINVD. Original Podman disk inode165146,
size99857989632, mtime1793953935 unchanged. Isolated machine restored1CPU/2GiB,
stopped, no remaining VM/gvproxy; takeover/vms/fast/hv_support all zero.

After activation37: normal vCPU --modes → VM --rom → fast-test including --sce;
then direct LinuxSMP2 and native PodmanSMP2. If successful test container workload,
uncached build/storage/ports with SMP and increased CPU counts, clean initialization,
repeat lifecycle, then Docker Desktop/backend acceptance. Do not mark SMP or the
project complete based on static/source/offline checks.

Installation verified: installer exit0, kmutil exit28/reboot required, AuxKC37 at the expected path, codesign valid. Loaded36; all four idle gates zero. Backup36 retained. Hardware37 pending manual reboot. See `runs/0.3.37-install.log`.
