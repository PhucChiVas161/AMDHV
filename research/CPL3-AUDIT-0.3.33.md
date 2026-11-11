# 0.3.33: native fast long-mode user contexts and privilege transitions

2026-11-11. Loaded baseline 0.3.32. New 0.3.33 hardware behavior is pending
reboot. Complete Linux boot and Podman/Docker containers remain unproved.

## Runtime evidence

Normal vCPU --modes, VM --rom and fast-test PASS on loaded 0.3.32:
`runs/0.3.32-{vcpu-test,vm-test,fast-test}.log`. This includes new full-layout
XSAVE/AVX-512 ZMM0/ZMM31/K7, PKRU/PKE and key fault tests, disabled guest
XCR0/OSXSAVE, reset isolation and host FP/XCR0 restoration.

Default Alpine Linux, without feature-clearing command line, reaches 1130
console bytes (drivers/protocols/certificates and initrd freeing), then fails
at 0.446s: `runs/0.3.32-vz-linux60.log`. CPUID exits at user RIP 7fb6ebdb3123;
VMM advances to 7fb6ebdb3125, but old mode validation rejects CS33/a0fb and
SS2b/c0f3. CR4=352eb0, XCR0=e7, EFER=d01. FS=7fb6ebf44b28; null data caches,
busy TR40/basefffffe0000003000/limit4087. Cleanup restores all idle gates to0.
This is evidence of executed user code, not complete init/login or container boot.

## Grouped implementation and audit

- Fast active 64-bit code supports CPL0/3 only, flat GDT code/data, consistent
  CS.RPL/CS.DPL/SS.RPL/SS.DPL, canonical FS/GS and validated TSS for user mode.
  Rings1/2, LDT selectors, conforming code, user null SS and guest TF/IOPL remain
  rejected. Legacy/step modes retain their previous restrictions.
- Explicitly initialize VMCB.CPL from validated SS.DPL on EVERY entry. Merely
  restoring CS selector33 into a zeroed VMCB is insufficient.
- Publish SS.DPL from actual VMCB.CPL, including SYSRET's potentially stale
  SS cache DPL. Never synthesize segment presence/type or alter CS selector.
- Hardware handles IRETQ, SYSCALL/SYSRET, SWAPGS, TSS RSP0/IST, native exceptions
  and ordinary INTn gate privilege. Clear INTn intercept only in native fast64;
  intercepting before DPL checks would bypass architectural behavior.
- CR/DR/MSR/HLT/INVLPG privilege exceptions normally precede their intercepts.
  Defensive nonzero-CPL guard rejects unexpected privileged exits before any
  emulation/VMX forwarding. I/O remains allowed to exit only after hardware
  IOPL/TSS bitmap checks. Descriptor UMIP checks execute natively.
- XSETBV is the exception: SVM intercept precedes CPL #GP. User XSETBV returns
  VMX #GP when bitmap13 is set, otherwise queues guest #GP without advancing
  RIP or modifying XCR0. Pending injection survives quantum/cancel boundaries;
  clearing remains one-shot after an actual VMRUN. General CPL0 invalid-XSETBV
  exception synthesis remains outside this change.
- Software instruction fetch aggregates all guest PTE U/S bits. CPL3 requires
  a user mapping; SMEP prohibits user instruction pages only for supervisor
  fetch. NPT ownership and NX checks remain in force.

## Broader firmware and userspace review

VZG11.fd SHA256 remains
3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903.
Reuse unchanged 70-PE inventory `efi-feature-inventory-0.3.32-25G332.json`:
privilege-related linear sites include LIDT7, LGDT4, LTR1, IRETQ3, IRETD147,
INT169, INT3 7652 and SYSRET15. Many sites are padding/embedded-data/mixed-mode
false positives; these counts are NOT reachable-path or execution proof.
TE/raw/dynamically generated code remains outside that inventory.

`linux/initramfs-cpl3-audit-0.3.33.json` records hashes and executable PT_LOAD
linear decode for extracted busybox, musl loader and libcrypto from the actual
initramfs. Musl has503 SYSCALL sites; libcrypto has10 CPUID,1 XGETBV,6 RDTSC.
The static scan does not identify the process behind the observed ASLR RIP.
It also does not prove every instruction along any runtime path is supported.

## Verification and next runtime gate

`runs/0.3.33-offline-checks.log`: build/imports339, ASan/UBSan units including
CPL policy and user instruction fetch, four actual FP-switch variants,
resolver6 and script syntax all PASS. Unit coverage includes the published
Alpine segment/cache shape, inconsistent privilege rejection, SS.DPL and
intercept classification. This is separate from hardware execution.

New `cplprobe` builds/signs and statically disassembles embedded handlers:
`runs/0.3.33-cpl-fixture-disassembly.txt`. Planned hardware assertions include
real IRET to user mode and raw cache re-entry, TLS/FSGSBASE, SYSCALL/SYSRET and
SWAPGS, denied privileged instructions/XSETBV/UMIP, TSS allowed/denied I/O,
user page permission and NPT ownership faults, external IRQ/IST, INT80/INT81,
INT3, and unowned privilege-stack NPF with interrupted IRQ vectoring/cleanup.

After reboot: normal vCPU --modes -> VM --rom -> fast-test (cplprobe last).
Only after all PASS run default Linux again without feature clearing.
APIC/SMP, compatibility-mode userspace, complete NMI/debug, type4 software-INT
reinjection after interrupted delivery, arbitrary controls and a complete
container lifecycle remain unverified or unsupported. Fast handled-exit quantum
is bounded, but execution duration still depends on host timer delivery.

## Architectural references

AMD APM volume2 rev3.38, table15-7 and sections15.10.2/15.11 describe
intercept/privilege ordering, INTn/XSETBV exceptions and I/O/MSR checks:
https://www.scs.stanford.edu/~zyedidia/docs/x86/amd-manual.pdf

Linux/KVM v6.12 svm_get_segment/svm_set_segment documents actual VMCB.CPL as
SS.DPL authority, including SYSRET behavior (independent implementation here):
https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/svm/svm.c

Staging confirmed: install exit0; kmutil exit28 requests reboot; AuxKC has0.3.33
at /Library/Extensions/AMDHV.kext. Backup0.3.32 saved. Loaded remains0.3.32;
takeover/vms/fast/hv_support all0. Stop at the reboot boundary.
