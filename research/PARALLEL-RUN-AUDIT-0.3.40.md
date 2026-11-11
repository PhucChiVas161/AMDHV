# Parallel-run and firmware audit — 0.3.40

2026-11-11. Hardware results use loaded0.3.39. Hardware40 remains pending.

## Verified39

- vCPU modes669 / VM ROM37 / fast271 PASS lines, zeroFAIL, all scripts exit0.
  Live exit-trace toggle, AP real-to64 transitions and NMI all PASS.
- Docker Desktop4.99.0/AppleVZ4CPU/4GiB: exact readonly host-reboot volume proof,
  full container/DNS/bind/volume/uncached build/published port/API workload, all
  CPU0..3 affinity hashes and normal stop PASS, exit0. Workload23.47s; nested
  affinity fixture5.13s includes container/package/network overhead, not a CPU
  benchmark. Docker engine reports ready16.436s, versus16.611s in the38 proof run;
  these samples do not establish a meaningful startup speedup.
  `runs/0.3.39-docker-smp4.log`.
- Podman clean2/4CPU/4GiB: **default guest cmdline, no tsc=reliable**, clocksource
  tsc at workload entry. Exact readonly host-reboot volume proof and the complete
  workload/all four affinity hashes PASS, normal stop, exit0. Workload40.37s,
  nested affinity fixture7.08s with the same overhead caveat. Watchdog remote-CPU
  and HPET-read timeout warnings still occur. One pass is not stability proof.
  `runs/0.3.39-podman-default-smp4.log` and retained console.
- Same default4 restart attempt entered high Linux RIPs but did not reach engine;
  serial log remained empty while trace showed repeated spin-loop quanta.
  No second VM/Docker interference observed. Requested Stop, then HardStop of
  this owned disposable vfkit through its REST endpoint. Provider script exit125
  (vfkit exited unexpectedly0 after deliberate HardStop), cleanup/gates0. This is
  a canceled repeat, not a guest crash/root-cause diagnosis or another PASS.
  `runs/0.3.39-podman-default-smp4-repeat*`. Clean2 restored2CPU/stopped.
- New smpprobe on39: two guests each set their own flag, wait for the other flag
  without PAUSE, then CPUID. Both FIRST run calls reached CPUID in **0/64** trials,
  errors0. Serialized baseline PASS. This directly exercises the serialized-run
  limitation; it does not quantify its share of application startup latency.
- Same probe also protects/unmaps/remaps an unused data page8 times with two
  infinite guest code loops active, then destroys both threads and all mappings.
  Writer progress/teardown PASS on39. `runs/0.3.39-smp-serialization-baseline.log`.

##40 scope and lock/lifetime review

- Replace the global mutex with an IORWLock, using SDK-declared/exported
  lck_rw_lock/unlock_shared/exclusive and IORWLockAlloc/GetMachLock (imports345).
- Default `debug.amdhv.parallel=0`: run calls still exclusive. Set1 only with
  takeover disabled and zeroVMs. Fast mode is also required for shared run leases.
  No shared step-mode runs. Boot remains CPUID-only, fast/takeover/parallel0.
- Shared path is ONLY thread traps0/1 (run / run-until FOREVER), through the
  existing owning-thread lookup. Lookup, pull, run and publish all stay inside
  the lease. No target pointer survives an unlock/relookup gap.
- Every create/destroy, task/thread destruction callback, MSR/control trap,
  map/protect/unmap, admission change is an exclusive writer. They drain every
  active run before changing metadata or releasing pinned backing/shared ABI.
  VM-count inspection is a reader; it does not mutate metadata.
- Only the owning host thread accesses each VcpuState/VMCB/GPR/FP/XSAVE buffer.
  Published VMCS/FP/MSR slots have existing separate per-vCPU strides. VM registry,
  CPU arrays, taskGone, region/live lists, pin totals and NPT topology stay writer
  controlled. Cancellation consumes per-vCPU atomic bits; interrupt trap retains
  its independent mutex. Lock order stays backend RW lease -> interrupt mutex;
  interruptTask never takes the backend lease.
- Each VMRUN quantum keeps host state on one physical CPU until restoration.
  ASID1 is CPU-local; initial full flush remains on EVERY quantum, with CR/INVLPG
  invalidation unchanged. Writers cannot revoke NPT backing until all runs leave;
  later runs flush before reuse. No shootdown relaxation or ASID allocation change.
- Guest memory read descriptors now request kIOMemoryThreadSafe for both normal
  writable pins and read-only fallback pins. XNU readBytes/getPhysicalSegment uses
  its IOMemory lock when this flag is present. Primary implementation reference:
  https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOMemoryDescriptor.cpp
  (source reference, not a claim that main exactly matches installed25G332).
  Local SDK declares flag00100000; tests mask direction separately from options.
- Native fast INT/IRET/event frames and guest page-table A/D updates execute in
  hardware. Step EventMemoryPlan writes remain exclusive. Unexpected fast IRET
  intercept now fails closed rather than invoking step software PTE/frame stores
  under a shared lease. Guest instruction/far-pointer diagnostic walks read only
  and never commit A/D updates. No new permissions or exception acceptance.
- Read-only diagnostics may observe a concurrently changing guest instruction or
  page-table entry; they remain bounded to owned pinned mappings and are evidence,
  not a snapshot or repair mechanism. No guest address becomes a raw host pointer.
- No changes to host IRQ/GIF/assembly/CR2/DR/EFER/HSAVE restoration, FP policy,
  NMI capabilities, timer emulation, TSC offsets/ratios or run-until support.

## Broad firmware grouping

Re-extracted installed25G332 VZG11.fd, inventoried70 PE modules.40 and39 match
by module SHA, names, instruction sites/counts and aggregate categories. Extractor
paths differ for5 modules and output ordering differs; comparison deliberately
normalizes path/order, not firmware bytes. See `runs/0.3.40-firmware-compare.log`.
Linear executable-section inventory can include data and excludes TE/raw/generated
code; it is not full runtime reachability proof.

Reviewed AP real/protected/compatibility/64 transitions, CR3/PAE/LME/NXE/SCE, GDT/
IDT/TSS, far jump and shared exchange, native atomics/PAUSE/CPUID, IRQ/NMI/IRET,
guest paging/NPT and host state switching as one group. New sharing does not
emulate or rewrite firmware synchronization. The intermittent EFI GP/PF has not
been reproduced with enough runtime evidence to claim a fix.

## Validation and next activation

- Offline40 build/sign/imports345, memory/state/mode/MSR/interrupt/CR/IO/debug/event/
  xstate/CPL ASan/UBSan, four FP variants, resolver6 and script syntax PASS.
  `runs/0.3.40-offline-checks.log`. Final smpprobe rebuilt and its serialized
  baseline/writer phase hardware-tested on39. All scripts syntax checked after
  adding explicit FIXVM_PARALLEL=0/1 selection and saved-flag cleanup.
- Disassembly: MFENCE remains; no host INVD/WBINVD/WBNOINVD. Shared state/FP and
  cancellation policy tests pass; these do not replace hardware40 validation.
- After activation run serial gates first, then `fast-test.sh --parallel` on the
  SAME40. Parallel smpprobe requires positive first-call overlap and writer/teardown
  success. Do not run providers with parallel1 unless those pass.
- Both provider harnesses defaultparallel0. Explicit `sudo env FIXVM_PARALLEL=1`
  selects the experiment before takeover and restores the previous flag after
  teardown. Podman startup now records external time including provider readiness.
  Serial/parallel comparison after activation requires only guest restarts.

Hardware40 concurrency, startup benefit, default SMP stability and full project
acceptance remain open. Backup39 and activation checkpoint recorded separately.
