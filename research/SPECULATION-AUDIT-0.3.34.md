# 0.3.34: speculation-control commands and hardware guest SPEC_CTRL

2026-11-11. Loaded baseline0.3.33; new kernel behavior pending reboot.
Goal remains native Podman/Docker. No complete Linux/container boot yet.

## Proven runtime progress and next failure

Loaded0.3.33 passes normal vCPU --modes (630 PASS lines), VM --rom (29),
fast-test (172), including real IRET3, restored CPL3, TLS/FSGSBASE,
SYSCALL/SYSRET/SWAPGS, native GP/user page permissions, TSS I/O bitmap,
IRQ RSP0/IST, INT80/INT81 DPL checks, INT3 and interrupted-delivery NPT.
Logs `runs/0.3.33-{vcpu-test,vm-test,fast-test}.log`.

Two userspace fixture mistakes were corrected on the SAME loaded kext:
VM creation needs HV_VM_DEFAULT (relative-TSC flag2 belongs to vCPU creation),
and AMD SYSRET's STAR user selector must include RPL3 (0023000800000000).
AMD does not force SS.RPL3. Original logs retained as
`0.3.33-fast-test-fixture-{options,star}.log`; no kernel relaxation/normalization
was used to make these tests pass. Nonconforming raw SYSRET caches remain
rejected by the strict input policy.

Default Linux, unchanged command line, survives a60-second VM lifecycle and
prints5157 console bytes, but has a real guest kernel Oops at0.275s:
`runs/0.3.33-vz-linux60.log`. It executes /init, userspace fork and SYSCALL;
then switch_mm_irqs_off+4a9 performs WRMSR with ECX49, EDX:EAX1 and gets GP.
This is PRED_CMD/IBPB while switching address spaces. VM Running/lifecycle PASS
is not proof of a healthy guest: the guest sits in its fault/panic path.

Explicit `mitigations=off` diagnostic reaches Alpine Init3.12.0-r0, loads boot
drivers, waits for absent boot media and launches its emergency recovery shell
at5.34s: `runs/0.3.33-vz-diag-mitigations-off.log` (10-second observation).
This isolates the mitigation group; it is not the project's default supported
configuration or a normal rootfs/container boot.

Userspace vzprobe now detects Oops/Kernel panic/unhandled page-fault markers
and returns failure even when the VM lifecycle survived. Replayed default
1-second run correctly returns1 and detects the same Oops:
`runs/0.3.33-vz-linux1-consolecheck.log`. This is console pattern detection,
not a comprehensive guest health/readiness check.

## Broad feature audit before staging

Raw physical CPUID `runs/0.3.33-speculation-cpuid.log`:
80000008.EBX791ef257; 8000000a.EDXfebfbdff; 80000021.EAX593fffef;
7.0.EDX10000110. Derived known SPEC_CTRL bits87, PRED_CMD bits81,
hardware V_SPEC_CTRL and L1D_FLUSH available. These are physical prerequisites,
not a claim about every feature exposed by Apple's VMM to the guest.

| Group | New behavior / boundary |
|---|---|
| PRED_CMD49 IBPB0/SBPB7 | CPL0 fast64 only; validate full EDX:EAX against raw physical feature bits, execute a real physical WRMSR for nonzero command; zero is valid/no action |
| SPEC_CTRL48 IBRS0/STIBP1/SSBD2/PSFD7 | Requires physical V_SPEC_CTRL plus each individual capability; guest-private value in VMCB6e0, hardware host/guest OR model; never replace host controls with guest value |
| FLUSH_CMD10b L1D0 | Capability CPUID7.EDX28 required; validate full value and execute actual physical flush; zero is valid/no action |
| Invalid accesses | Write-only command reads, unavailable commands or reserved bits deliver GP(0), honoring VMX bitmap13 or native event injection without RIP advance |
| CPL3 | Existing hardware privilege checking and defensive intercept guard precede all new emulation; new PRED_CMD/SPEC_CTRL user tests added |
| Prefixes/quantum/cancel | Valid handled MSRs use hardware NRIP, checked before effects; pending GP survives bounded quantum/cancellation; command does not become a persistent value |
| Reset/re-entry/host state | Private specCtrl resets0, persists through API/VMRUN; kernel compares host SPEC_CTRL after each quantum and restores/fails if unexpectedly changed |
| Other mitigation controls | VIRT_SPEC_CTRL c001011f, model-specific LS_CFG c0011020, EFER.AUTOIBRS and unknown/new control bits are not silently implemented or accepted; prior forwarding/validation boundaries remain |

The current CPU does not advertise legacy VIRT_SSBD in80000008.EBX25.
Stateful SPEC_CTRL support is gated to hardware V_SPEC_CTRL; no software host
MSR swapping path is introduced for older CPUs. Existing shared ABI, managed
MSR IDs and assembly FP/HSAVE transitions remain unchanged.

Exact Alpine bzImage ELF64 executable PT_LOAD scan finds candidate immediate
ECX sites for SPEC_CTRL13, PRED_CMD1 and EFER5:
`linux/speculation-msr-audit-0.3.34.json`. Preceding6-instruction candidate scan
is not dataflow/reachability proof and may include alternative code/data.

Firmware SHA256 remains
3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903.
Reuse unchanged70-PE inventory, isolate91 RDMSR/WRMSR sites in
`efi-msr-sites-0.3.34-25G332.json`. These sites do not resolve all ECX values;
linear executable decode excludes TE/raw/dynamic code and can contain data.
No claim of complete firmware/MSR coverage or a reduced reboot count.

## Verification

`runs/0.3.34-offline-checks.log`: full build/imports339, ASan/UBSan, all mode/
MSR/CPL/xstate/etc units, four actual FP-switch variants, resolver6 and shell
syntax PASS, no compiler warnings. Speculation units exercise physical feature
masks, all256 low-byte control/command values, full-width invalid inputs,
write-only semantics, absent virtualization and reset isolation.

`runs/0.3.34-vcpu-svm-disassembly.txt` records compiled paths. Physical WRMSR is
reachable only for the classified command addresses49/10b after mask/privilege
and NRIP checks; its inline asm has a memory clobber. Normal SPEC_CTRL writes
update guest VMCB only. Host48 write exists solely for defensive restoration
after a detected mismatch, before host IRQ reenable. VMCB clean bits are reset
before each VMRUN. No new assembly stack offsets are introduced.

New signed `specprobe` covers guest controls0/individual/all bits, readback and
32 repeated entries, prefixed WRMSR/NRIP, valid zero/IBPB/SBPB/L1D commands,
native and bitmap GP, reserved/high32 bits, persistent controls after commands,
and reset of a reused vCPU. CPL3 PRED_CMD/SPEC_CTRL probes were added separately.
**New hardware behavior is pending loaded0.3.34; offline success is not proof
of barrier effectiveness or complete host/guest side-channel isolation.**

After reboot: normal vCPU --modes -> VM --rom -> fast-test (specprobe last).
Only after all PASS run default Alpine without mitigations=off/feature clearing;
inspect console and actual init state, then proceed toward a disposable normal
rootfs and the native container backend. Existing Podman disks remain untouched.

## Primary architecture/reference sources

AMD APM volume2 rev3.38 sections3.2.9 and15.37 specify capability-gated
SPEC_CTRL/PRED_CMD and hardware guest/host controls:
https://www.scs.stanford.edu/~zyedidia/docs/x86/amd-manual.pdf

AMD APM volume3 SYSRET explains SS.RPL not being forced3:
https://www.scs.stanford.edu/~zyedidia/docs/x86/amd-manual-v3.pdf

Linux/KVM v6.12 supplies reference offsets/features and command validation
(independent implementation here; no KVM code copied):
https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/svm.h
https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/cpufeatures.h
https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/x86.c
https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/svm/svm.c
https://github.com/torvalds/linux/blob/v6.12/arch/x86/mm/tlb.c

Staging confirmed: install exit0; kmutil exit28 requests reboot; AuxKC contains
0.3.34 at /Library/Extensions/AMDHV.kext. Backup0.3.33 retained. Loaded remains
0.3.33, takeover/vms/fast/hv_support all0. Stop at the reboot boundary.
