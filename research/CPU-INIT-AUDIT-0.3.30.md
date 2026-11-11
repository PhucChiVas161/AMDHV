# 0.3.30: native CPU-init feature group

2026-11-10. Runtime evidence collected on loaded AMDHV0.3.29. New0.3.30 hardware
behavior remains pending reboot; offline PASS is not a hardware result.

## Confirmed baseline and Linux progress

Normal vCPU --modes, VM --rom and fast-test all PASS without diagnostic repairs:
`runs/0.3.29-{vcpu-test,vm-test,fast-test}.log`. This includes every new0.3.29 fault
test: native PF/UD/GP, retry/IRET, bitmap exits, CR2, error-code reinjection,
constant PF filters, vectoring NPF and triple-fault VMX2. Finite fast workload
observed13 physical host IRQ exits, worst call8.524ms in this run only.

Default VZ Linux advanced beyond the old REP MOVSD page fault into CPU setup,
then rejected MOV CR4 atffffffff81060f16, operand100b0 (FSGSBASE). Log
`runs/0.3.29-vz-linux60.log`, failure after0.132s, console0bytes, clean teardown.
This does not prove complete Linux boot or a working container.

## Broader diagnosis before staging

The new explicit `FIXVM_LINUX_EXTRA_CMDLINE` probe variable prints its value and
only changes the disposable Linux guest's command line. The default command line
is unchanged. No Podman disk, guest kernel binary or loaded kext was modified.

| Diagnostic command-line suffix | Actual next boundary | Log under runs/ |
|---|---|---|
| nofsgsbase | CR4=1000b0, SMEP | 0.3.29-vz-diag-nofsgsbase.log |
| nofsgsbase nosmep nosmap clearcpuid=umip | Same SMEP boundary; these flags did not remove that feature in this path | 0.3.29-vz-diag-cr4.log |
| clearcpuid=fsgsbase,smep,smap,umip | CR4=406b0, OSXSAVE | 0.3.29-vz-diag-clearcpuid.log |
| clearcpuid=fsgsbase,smep,smap,umip noxsave | INT3 atffffffff82e43fec, SVM exit43 | 0.3.29-vz-diag-cr4-noxsave.log |

All four are diagnostic FAIL results, not normal compatibility gates. The last
INT3/four-NOP sequence and following value check match Linux `int3_selftest()`;
the evidence does not identify a guest kernel panic. Exact ELF address mapping
and disassembly are in `linux/alpine-cpu-init-audit-0.3.30.txt`.

Primary references:

- [Linux v6.12 CPU initialization](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/cpu/common.c)
  groups SMEP/SMAP/UMIP and FSGSBASE setup; early clearcpuid parsing is a useful
  diagnostic before setup. Alpine is6.12.115; source correspondence is an
  inference from matching binary behavior, not an exact debug-symbol lookup.
- [Linux FSGSBASE documentation](https://docs.kernel.org/arch/x86/x86_64/fsgs.html)
  identifies leaf7 EBX0 and explicit CR4 enablement for the four base instructions.
- [Linux INT3 self-test](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/alternative.c)
  deliberately executes INT3 and redirects execution through its notifier.
- [Linux xstate setup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/xstate.c)
  enables OSXSAVE and programs XCR0. That requires a separate extended-state switch.
- [Intel system programming manual](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf)
  documents CR4.FSGSBASE/SMEP/SMAP and guest permission semantics.

Firmware VZG11.fd SHA256 remains
3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903.
Re-extracted and rescanned70 PE modules with new FSGSBASE/STAC/CLAC category;
all70 PE hashes match the previous inventory. No sites in that added category
were decoded. `efi-feature-inventory-0.3.30-25G332.json` retains per-module sites.
This linear executable-section scan may decode data/mixed-mode bytes, excludes
TE/raw modules and does not prove reachability or absence of dynamic code.

## Implementation and boundaries

| Group | 0.3.30 behavior |
|---|---|
| FSGSBASE/SMEP/SMAP/UMIP | Native guest CR4 bits allowed only in fast code64; physical leaf7 support checked before entry and on emulated CR4 writes |
| VMX CR masks/shadows | Masked writes still exit before changing private state; VMXE remains virtual and is never put into AMD CR4 |
| FS/GS/KERNELGSBASE | Existing VMLOAD/VMSAVE guest/host swap and managed-MSR synchronization preserve aliases; no host base writes added |
| SMAP AC | Fast code64 may retain AC across exits; hardware STAC/CLAC and guest permission checks remain native |
| Intercepted-code fetch | Read-only software walker also checks effective U/S through all paging levels for CPL0 SMEP, including huge pages |
| INT3 | Unmasked native fast-code64 delivery uses hardware post-instruction RIP and guest IDT; bitmap3 remains intercepted/Unsupported, VMX type6 reinjection rejected |
| OSXSAVE/XCR0/AVX | Still fail closed. Existing guest512-byte FXSAVE and host legacy-mask XSAVE cannot safely preserve extended guest mutations |

No guest CPL3, PCID/no-flush CR3, LA57, PKRU/CET, full debug/NMI or general
software-interrupt ABI is added. No FP switching or host IRQ assembly changed.
OSXSAVE is a known remaining default-Linux blocker found before this reboot,
not a newly discovered surprise to be counted as Linux boot success.

## Validation

- Unit: physical feature masks; fast/step/code32/compatibility restrictions;
  AC, unsupported OSXSAVE, mask/shadow write priority and atomic rejections;
  SMEP U/S aggregation at every level and4K/2M/1G leaves; native vs intercepted BP.
- New faultprobe hardware cases: native INT3/IRET saved RIP; fail-closed intercepted
  BP; guest CR4 group enable/readback; WRFSBASE/WRGSBASE/RDFSBASE/RDGSBASE,
  FS/GS memory operands, SWAPGS/KERNELGSBASE and managed-MSR re-entry;
  disabled-FSGSBASE UD and noncanonical GP; CPL0 SGDT with UMIP; SMEP user fetch
  PF; SMAP user read PF; STAC exit/AC re-entry/CLAC; VMX masked CR4 exit;
  pre-entry OSXSAVE rejection. **All these additions await loaded0.3.30.**
- Offline PASS: build, imports339, ASan/UBSan, actual FP switch preservation,
  resolver6cases and script syntax. `runs/0.3.30-offline-checks.log`.

After reboot run normal vCPU --modes → VM --rom → fast-test. Only if all PASS,
try default Linux again; if OSXSAVE is still the boundary, the explicitly labeled
`noxsave` diagnostic can exercise the new native feature group and INT3 path
while extended-state support is developed. It is not the final Podman/Docker setup.

Installation complete: AuxKC contains0.3.30 at /Library/Extensions/AMDHV.kext,
install script exit0 / kmutil exit28 (reboot required). `runs/0.3.30-install.log`.
Loaded remains0.3.29; takeover/vms/fast/hv_support all0. Prior installed0.3.29
backed up at `kext-backups/AMDHV-0.3.29.kext`. Stop at reboot boundary.
