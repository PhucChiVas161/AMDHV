# 0.3.32: CPUID-sized AVX-512 and protection-key state

2026-11-11. Runtime baseline: loaded0.3.31. New0.3.32 hardware behavior remains
pending reboot. No complete Linux/container lifecycle has been demonstrated.

## Confirmed baseline and diagnostics

Normal vCPU --modes, VM --rom and fast-test all PASS:
`runs/0.3.31-{vcpu-test,vm-test,fast-test}.log`. This proves the new0.3.31
privileged XCR0=1/3/7 transition, AVX, guest standard/compacted XSAVE, reset
isolation, invalid-input rejection and host x87/XMM/YMM/XCR0 restoration.
The finite fast workload observed71 host IRQ exits, worst call2.899ms in that
run only; this is not a hard latency guarantee.

Default Linux reaches XSETBV atffffffff8105a2a7, NRIPffffffff8105a2aa,
CR4=350eb0, exception bitmap0, then fails after0.170s with console0bytes.
`runs/0.3.31-vz-linux60.log`. The old error log did not record EDX:EAX/ECX,
so the exact requested XCR0 is not a runtime measurement.

- Explicit `clearcpuid=pku` diagnostic still fails at that XSETBV,0.160s:
  `runs/0.3.31-vz-diag-nopku.log`.
- Explicit `clearcpuid=avx512f,pku` keeps XSAVE enabled and reaches XCR0=7,
  prints1130 console bytes, initializes drivers/protocols and frees initrd,
  then fails at CPL3 (CS33/a0fb, SS2b/c0f3, RIP7fe221c08125),0.449s:
  `runs/0.3.31-vz-diag-xstate7.log`.

The differential result identifies the broader xstate capability group as the
default path's blocker; it does not prove the unlogged numeric mask. New failure
logging records rejected XSETBV value/index/CPL/supported mask. The diagnostic
console is not complete init/login or default Podman/Docker compatibility.
CPL3 remains a known separate boundary rather than an unexpected future claim.

## Physical layout and grouped policy

Measured `runs/0.3.31-full-xstate-layout.log`:

| Component | XCR0 bit | Standard offset | Bytes |
|---|---:|---:|---:|
| YMM upper halves | 2 | 576 | 256 |
| Opmask | 5 | 832 | 64 |
| ZMM0–15 upper256 | 6 | 896 | 512 |
| ZMM16–31 | 7 | 1408 | 1024 |
| PKRU | 9 | 2432 | 8 |

Physical mask2e7, total2440 bytes; host XCR0 currently7. The implementation does
not hard-code this whole layout: physical CPUID.D subleaves supply offsets/sizes.
Only x87/SSE/AVX, the complete three-component AVX-512 group and PKRU are selected.
Mandatory x87, SSE/AVX/AVX-512 dependencies and physical capability subsets are
validated for both host and guest masks. Each selected standard-layout component
must be nonzero, nonoverlapping, not supervisor-managed and end below3968.
The established AVX576/256 contract is checked explicitly. Unknown states,
AMX/CET/supervisor state and oversized/inconsistent layouts stay rejected.

The legacy framework FP API remains512 bytes. A per-vCPU private3584-byte tail
completes a4096-byte standard-format buffer, reset to zero except legacy-state
valid bits. Legacy rewrites retain extended state. No extended bytes are mapped
to the guest or copied to the framework's legacy FP window.

CR4.PKE is admitted only for fast code64 with physical PKU and a switched PKRU
component. Native RDPKRU/WRPKRU and PTE protection keys act on guest state. PKRU
must be saved even when guest XCR0[9] or guest OSXSAVE is clear: protection keys
are not themselves XSAVE-enabled instructions. All guest MSR writes remain
intercepted; host XSS/XFD0 guards remain in place.

Primary architectural references:

- [Intel system programming manual](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf)
  specifies AVX-512 XCR0 dependencies and PKRU's user-page access rights.
- [Intel instruction-set extensions reference](https://www.intel.com/content/dam/develop/external/us/en/documents/319433-024-697869.pdf)
  describes CPUID.D size/offset enumeration and xstate components.
- [Intel SDM documentation changes](https://cdrdv2-public.intel.com/789573/252046-sdm-change-document.pdf)
  identifies PKRU as component9, independently usable from its XSAVE enablement.
- [Linux XCR0 validation](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/x86.c)
  enforces the mandatory x87 and complete AVX-512 group dependencies.

## Full physical-state transition

Saving only host XCR0=7 before enabling guest AVX-512/PKRU would leave physical
state outside that mask unprotected. Therefore the switch always saves/restores
all selected physical components, including those disabled by the host's current
XCR0, rather than only the guest's active mask.

Within the existing GIF0/CLTS window:

1. Preserve the guest buffer pointer before ECX is used as the XCR index.
2. Temporarily expand XCR0 to the validated physical switch mask; XSAVE all
   selected original host state, then XRSTOR all private guest state.
3. Apply the validated guest XCR0 (or3 when guest OSXSAVE is off), then
   VMLOAD/VMRUN. Guest XSETBV remains intercepted and validated before commit.
4. On every exit, CLI immediately follows VMRUN. After saving guest GPRs and
   VMSAVE/VMLOAD contexts, expand to the switch mask again, XSAVE guest state,
   XRSTOR all original host physical state, then restore the exact original
   host XCR0 before CR0/DR0/STGI and before host C++/interrupts resume.

Kernel buffers and the kernel stack are supervisor mappings. PKRU controls user
pages, so guest PKRU cannot deny these assembly memory accesses while the guest
rights are temporarily live. No host user pointer is dereferenced in that window.
Original physical PKRU and vector components are restored before returning to
host execution, including components that were disabled in the original XCR0.

Private host-buffer metadata: original XCR0 at4064, switch mask4072, guest mode/
XCR0 at4088. The3968-byte state bound leaves metadata outside the XSAVE image.
The stack layout and host-IRQ argument offset120 remain unchanged. Both entry
and exit use scratch R10 for the FP pointer before clearing ECX; guest R10 is
loaded afterwards on entry and already saved before exit restoration.
`xsave-transition-disassembly-0.3.32.txt` records compiled ordering.

## Broad audit and verification limits

Firmware re-extracted and rescanned70 PE modules. All PE hashes match the prior
0.3.30 inventory; added PKRU and ZMM/opmask categories decode0 sites.
`efi-feature-inventory-0.3.32-25G332.json`. This is a linear executable-section
inventory, may decode embedded data/mixed modes and excludes TE/raw/dynamic code;
it is not reachability proof or proof of complete firmware support.

- Unit covers physical subsets, valid/invalid AVX-512 groups, layout presence,
  duplicate/overlapping/overflowing components, supervisor rejection and metadata
  boundaries, plus PKE mode restrictions and VMX mask/shadow priority.
- Four actual FP data-transfer variants still PASS1000 mutations under
  ASan/UBSan. The shared dynamic-mask save/restore helper is exercised with the
  host-enabled mask7. These userspace tests do not enable privileged extra host
  XCR0 bits or prove ZMM/PKRU restoration.
- New xsaveprobe hardware additions: full guest XSETBV, ZMM0/ZMM31 and K7 across
  re-entry and legacy FP rewrite; full-layout guest XSAVE/XRSTOR; native PKE and
  PKRU persistence with guest XCR0[9]/OSXSAVE disabled; key1 access-disable and
  write-disable guest PF.PK; recovery on clearing PKRU; reused-vCPU PKRU reset.
  Emitted bytes reviewed in `xsave-fixture-disassembly-0.3.32.txt`.
- Existing hardware wrapper checks original host XCR0 and x87/XMM/YMM/init
  state after successful and rejected runs. It does not directly read host PKRU
  or disabled host ZMM components from userspace; their restoration relies on
  the all-component transition and awaits the new hardware gate.
- Offline0.3.32 PASS: build/imports339, ASan/UBSan, four FP variants, xstate layout
  tests, resolver6cases and shell syntax. `runs/0.3.32-offline-checks.log`.
- **All new privileged expansion, AVX-512 and PKE/PKRU hardware cases remain
  pending loaded0.3.32.** No new hardware success is inferred from offline PASS.

After reboot: normal vCPU --modes → VM --rom → fast-test. Only if all PASS run
default Linux without feature-clearing diagnostics. CPL3/privileged-exit policy,
APIC/SMP and complete Podman/Docker lifecycle remain separate work.

Installation PASS: AuxKC0.3.32 at /Library/Extensions/AMDHV.kext; install script
exit0, kmutil exit28 (reboot required). `runs/0.3.32-install.log`. Loaded remains
0.3.31; takeover/vms/fast/hv_support all0. Previous installed kext saved at
`kext-backups/AMDHV-0.3.31.kext`. Stop at reboot boundary; new hardware tests pending.
