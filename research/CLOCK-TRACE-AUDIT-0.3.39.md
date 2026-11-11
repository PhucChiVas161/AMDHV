# Clock, AP transition and exit tracing audit — 0.3.39

Date: 2026-11-11. Hardware results below use **loaded0.3.38**.
Source/build39 changes only successful-exit logging and adds a userspace probe.
Hardware39 results remain pending activation.

## Results on38

- vCPU modes669, VM ROM37, fast267 PASS lines, zeroFAIL; all three scripts exit0.
  NMI injection/blocking/IRET/repeated NMI/nested external IRQ hardware cases PASS.
- Docker Desktop4.99.0, Apple VZ4CPU/4GiB: exact read-only volume proof after host
  reboot PASS before any write, normal stop. `runs/0.3.38-docker-reboot-volume.log`.
- Podman applehv clean2/2CPU/4GiB: same exact read-only host-reboot proof PASS,
  normal stop. `runs/0.3.38-podman-reboot-volume.log`.
- First default Podman4 attempt passed EFI into Linux but waited for root UUID.
  TSC watchdog reported HPET226495ns versus TSC197440ns, switched to HPET.
  Docker auto-opened at11:21:25 while this VM was running; CLI stop timed out,
  TERM of the identified backend shut Docker down. This is **not a clean isolated
  default4 acceptance result**. Podman was subsequently canceled and hard-stopped
  through its own vfkit REST endpoint; backend gates returned0. Console/live
  traces retained in `runs/0.3.38-podman-efi-smp4*`.
- Diagnostic comparison: queue `tsc=reliable` through rpm-ostree in disposable
  clean2, boot4CPU, verify actual cmdline and current clocksource=tsc. Exact prior
  volume proof, container/DNS/bind/volume/uncached build/localhost/API, strict
  affinity hashes on CPU0..3 PASS, including normal provider stop; script exit0.
  `runs/0.3.38-podman-tsc-{prepare,compare4}.log`. The6.33s affinity fixture includes
  package/network/container overhead; it is not a CPU benchmark.
- Diagnostic argument deletion was queued **before** the comparison workload.
  Subsequent default2 boot verified /proc/cmdline AND queued rpm-ostree args have
  no tsc=reliable; read-only exact volume proof and normal stop PASS, script exit0.
  `runs/0.3.38-podman-default-restored.log`. Clean2 left stopped2CPU/4GiB.
- Physical CPUID80000007.EDX=6799 reports invariant TSC.24 host threads sampled
  240000 serialized reads across APIC bitmapfff0fff with zero backwards values;
  ~4391.727MHz against mach continuous time, host sysctl4391.742MHz. The mach
  clock can derive from TSC: this is not an independent frequency reference or
  a proof of guest per-vCPU synchronization. `runs/0.3.38-host-tsc-sample.log`.

The comparison supports investigating guest clock/watchdog/MMIO timing. It does
not prove TSC is the only cause, that the intermittent EFI fault is fixed, or that
disabling the watchdog is a final compatibility solution. Docker's own default
Linuxkit command line already includes tsc=reliable. Linux's tsc=reliable option
sets tsc_clocksource_reliable: primary reference is
https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/tsc.c . This reference
explains the option, not the exact behavior of the guest's newer7.2.7 kernel.

## Broad firmware and grouped review

- Re-extracted installed25G332 VZG11.fd; static inventory39 equals38 in full:
  same70 PE modules, SHA256 and instruction-group inventories. Inventory scope
  remains executable-section linear decode, including possible embedded data;
  it excludes TE/raw/generated code and is not a reachability proof.
- Reviewed CpuDxe AP real prefix, segment loads/GDTR/IDTR, CR4.PAE, CR3 exchange,
  EFER.LME/NXE/SCE, masked CR0.PG and indirect FF2F compatibility far jump64.
  New `aplongprobe` uses three simultaneous independent AP threads/slots1..3,
  keeper0, private owned identity tables and far pointer/GDT.64 transitions each,
  alternating NXE/SCE preparation, full-width64 marker and final CR0/CR3/CR4/EFER
  checks. It models VMM handling of EFER and CR0, not the whole firmware and not
  the original EFI shared exchange area/timing. Hardware on38 passes; this
  narrows coverage gaps without claiming to reproduce or fix the EFI failure.
- Reviewed timer/TSC commpage tuple0,0, relative-TSC offsets, managed TSC writes,
  run-until FOREVER support, cancellation and fast PAUSE quantum exits. No clock
  ratio, offset, interrupt, timer, ASID/TLB, locking or execution policy changed.
- Reviewed all klog call sites. Trap tracing already stops after400 calls, but
  successful SVM exits always formatted, called IOLog and acquired gLogLock.
  Spin-loop traces overwrote the64KiB ring with repeated synthetic41/VMX1 exits.
  This is a concrete high-frequency logging path; its contribution to total
  latency has not yet been measured with39.

##39 change and checks

- In fast mode sample successful translated exits: first32 and powers of two
  per vCPU; always log native exception/VM shutdown exits. Rejected states and
  unsupported code/GPR/far-pointer/GDT diagnostics remain unconditional.
- New `debug.amdhv.trace_exits=1` enables full successful-exit tracing live;
  default0 samples fast exits. Step-mode trace remains full. Atomic flag changes
  only logging; no VM lock/host IRQ/assembly changes or new private kernel import.
- Per-vCPU release reports translated-exit/sample counters. Early cancellations
  that return before translation are outside those counters.
- fast-test includes invalid flag rejection, live full/sample toggle and the
  new AP probe. Cleanup restores the previous trace flag.
- Offline build/signature/imports339/ASan/UBSan/four FP variants/resolver6 checks
  PASS: `runs/0.3.39-offline-checks.log`. Hardware trace flag and reduced-log
  performance comparison require activating39; no performance claim yet.

Remaining: clean default Podman4 repetition, intermittent EFI fault evidence,
clock correctness, repeated full workloads and realistic performance/concurrency.
The full project goal is still open.
