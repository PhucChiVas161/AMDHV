# 0.3.31: bounded XSAVE/XCR0 transition

2026-11-11. Runtime evidence is from loaded0.3.30. New0.3.31 hardware tests
remain pending reboot; offline assembly-data tests do not prove privileged XCR0
switching, VMRUN ordering or a Linux boot.

## 0.3.30 hardware results

Normal vCPU --modes and VM --rom PASS. Initial fast-test failed only because the
new SMEP fixture expected PFEC15 instead of11: CPL0 fetch of a present user page
sets P+I/D, not U/S. This is also the Intel architecture, not an AMD-to-Intel
conversion requirement. Corrected only the userspace expectation; kext unchanged.
The full fast gate then PASS, including native INT3, FS/GS/SWAPGS, UMIP CPL0 SGDT,
SMEP, SMAP and AC re-entry. Original failed log preserved as
`runs/0.3.30-fast-test-initial-fixture-fail.log`; final logs
`runs/0.3.30-{vcpu-test,vm-test,fast-test}.log`. Finite workload observed33 host
IRQ exits and worst call5.390ms in this run, not a hard deadline guarantee.

[Intel SMEP specification](https://www.intel.com/content/dam/support/us/en/documents/processors/quark/sb/intelquarkcore_devman_001.pdf)
documents the supervisor-fetch PFEC10001b. [Linux fault handling](https://github.com/torvalds/linux/blob/master/arch/x86/mm/fault.c)
likewise distinguishes supervisor fetches from PFEC.U/S user accesses.

Default Linux now reaches CR4=350eb0 atffffffff81060f16, rejected only for
OSXSAVE; exception bitmap0. `runs/0.3.30-vz-linux60.log`.

Explicit diagnostic `FIXVM_LINUX_EXTRA_CMDLINE=noxsave` produces1130 console
bytes, initializes drivers/network protocols and frees initrd memory, then fails
at a CPL3 context: CS33/ARa0fb, SS2b/ARc0f3, RIP7ff66f87b125.
`runs/0.3.30-vz-diag-noxsave.log`, lifecycle failure after0.427s. This proves
guest kernel execution and console output, not complete init/login or container
success. CPL3 remains a known subsequent boundary; its segment validation,
privileged-instruction exits and syscall/return security need a separate audit.

## State/layout and capability audit

Measured `runs/0.3.30-xsave-layout.log`: host XCR0=7; physical CPUID.D.0 mask2e7,
enabled-size340(hex)=832; CPUID.D.2 size100(hex)=256, offset240(hex)=576.
Physical capability includes states the host OS has not enabled. They must not
be enabled for the guest merely because CPUID advertises them.

The framework FP window stays512 bytes. A private320-byte tail holds the standard
64-byte XSAVE header and256 bytes of YMM upper halves. It is per-vCPU, zeroed on
reset, never guest-mapped or published through the legacy API. Explicit legacy
FP rewrites mark only x87/SSE valid and retain the YMM tail. All private hardware
buffers remain resident/page-aligned and contain832 bytes of state; the run-mode
word at host-page offset4088 is outside that state.

Allowed guest XCR0 values are1/3/7: x87 is mandatory; AVX requires SSE. Step,
legacy and compatibility modes keep the old restricted policy. Fast code64 may
enable CR4.OSXSAVE and execute XSAVE/XRSTOR and bounded AVX. Entry requires host
CR4.OSXSAVE/OSFXSR, host XCR0 exactly7 and the physical AVX component layout
above. If physical XSAVES or XFD support exists, host IA32_XSS/IA32_XFD must be0.
This prevents guest XSAVES exposing host supervisor state or host XFD imposing
unexpected guest #NM. All guest MSR writes remain intercepted; this release
does not add native XSS/XFD/PKRU MSR mutation.

Primary references: [Linux XCR0 validation](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/x86.c)
checks mandatory x87, SSE/AVX dependencies and supported saved components;
[Linux xstate setup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/xstate.c)
programs OSXSAVE before XCR0. The implementation is an independent bounded
transition, not a port of the KVM FPU implementation.

## Grouped transition

1. Under GIF0 and CLTS, save all host x87/SSE/AVX with XSAVE mask7.
2. Restore all private guest components with XRSTOR mask7 while host XCR0 is
   still7. This initializes inactive components and prevents host YMM leakage.
3. Set the validated guest XCR0 (or3 if guest OSXSAVE is off), then VMLOAD/VMRUN.
4. On every VMEXIT, CLI remains immediately after VMRUN; save guest GPRs and
   VMSAVE/VMLOAD the segment/MSR contexts. Restore host XCR0=7 before guest XSAVE
   mask7 and host XRSTOR mask7. Only then restore exact host CR0/DR0 and STGI.
5. Guest XSETBV is intercepted: require code64/CPL0, OSXSAVE, ECX0, legal1/3/7,
   and decoded NRIP length3 before committing the virtual XCR0 and advancing RIP.
   Invalid/unsupported requests stop as Unsupported, without executing input on
   the host. This is not complete architectural #GP synthesis for invalid XSETBV.

The stack frame and host-IRQ argument offsets do not change. RCX, which holds
the guest FP pointer, is preserved in scratch R10 before XSETBV clears ECX on
the leave path. That scratch is used only after guest R10 has been saved. The
compiled ordering is recorded in `xsave-transition-disassembly-0.3.31.txt`.

Because all guest vector registers are restored/saved with mask7 under host
XCR0=7, changing guest XCR0 across entries cannot expose stale host state or lose
the private disabled-component contents. Legacy step runs keep the old FXSAVE
path and mark legacy state dirty before returning to the extended runner.

AVX-512, PKRU, CET, AMX, supervisor state, host XCR0 other than7 and nonzero host
XSS/XFD remain rejected. Guest XSAVES/XRSTORS compacted buffers are native guest
memory operations with XSS0; the backend's private state stays standard format.
No CPL3, APIC/SMP or final Podman/Docker compatibility is claimed by this change.

## Verification and next gate

- Unit validates XCR0 values, fast/step boundaries, CR4 masks, tail reset/isolation
  and legacy API rewrites without extending the shared512-byte window.
- Actual legacy FP macros still preserve host state across1000 guest mutations,
  including init-state tests. New actual XSAVE data-transfer macros preserve
  host x87/XMM/YMM/init flags and retain guest YMM mutations across1000 runs,
  both normal and init-state variants, under ASan/UBSan. These userspace tests
  exercise the real data-transfer macros, not privileged XSETBV.
- New xsaveprobe hardware gate covers guest CR4/XSETBV/XGETBV, AVX mutation,
  re-entry and legacy API rewrite, native guest XSAVE/XRSTOR, compacted
  XSAVES/XRSTORS where supported, XCR0 disable/re-enable, invalid API/guest
  requests, recreated-vCPU state isolation, and exact host FP/XCR0 restoration
  on success and rejection. Fixture bytes reviewed with Capstone in
  `xsave-fixture-disassembly-0.3.31.txt`.
- Offline0.3.31 PASS: build/imports339, ASan/UBSan, four FP switch variants,
  resolver6cases and shell syntax. `runs/0.3.31-offline-checks.log`.
- **All new privileged transition and XSAVE hardware gates await loaded0.3.31.**

After reboot: normal vCPU --modes → VM --rom → fast-test (xsaveprobe last).
Only if all PASS run default Linux without noxsave. Log the next actual boundary;
CPL3 and broader guest xstate requests are already known possibilities. Diagnostic
console progress must remain distinct from a successful Linux/container lifecycle.

Installation PASS: AuxKC0.3.31 at /Library/Extensions/AMDHV.kext; script exit0,
kmutil exit28 (requires reboot). `runs/0.3.31-install.log`. Loaded remains0.3.30;
takeover/vms/fast/hv_support all0. Backup `kext-backups/AMDHV-0.3.30.kext`.
Final probe additions also build/import-check clean in `runs/0.3.31-final-build.log`.
Stop at reboot boundary; no new-kext hardware result claimed.
