# Native backend milestone and NMI audit — 0.3.38

Date: 2026-11-11. Tested kernel: **loaded0.3.37**. Source38 hardware is pending.

## What actually passed on37

| Test | Result | Evidence in runs/ |
|---|---|---|
| vCPU --modes / VM --rom / fast-test | exit0,669/37/255 PASS lines, zeroFAIL | 0.3.37-{vcpu-test,vm-test,fast-test}.log |
| Direct Alpine6.12 SMP2 | CPUonline0-1, timers/IPIs/TLBshootdowns on both, four8MiB hashes match, guest poweroff | 0.3.37-vfkit-linux-smp2.log |
| Podman existing disposable clone,2CPU/2GiB | normal EFI/provider start/stop; container/DNS/bind/volume/uncachedbuild/localhost18080/API PASS | 0.3.37-native-podman-smp2-workload.log |
| Fresh Podman machine init,2CPU/4GiB/16GiB disk,fixvm-clean2 | actual init from cached pristine raw.zst, Ignition and full application workload, exit0 including stop | 0.3.37-clean2-podman-init.log; 0.3.37-clean2-podman-smp2-workload.log |
| Docker Desktop4.99.0/build245822,2CPU/4GiB | actual Apple VZ engine, container/DNS/bind/volume/uncachedbuild/localhost18081; normal stop | 0.3.37-docker-native-workload-v3.log |
| Docker same engine,4CPU/4GiB | full workload plus strict affinity hashes on CPU0,1,2,3, all four32MiB zero-input SHA256 correct; normal stop exit0 | 0.3.37-docker-native-smp4-strict.log |

Mac Podman CLI/guest engine5.8.9, Fedora CoreOS44/kernel7.2.7. Docker CLI/engine29.9.3,
Linux7.0.14-linuxkit. Actual Docker process is `com.docker.virtualization` with
`--cpus4 --memoryMiB4096 --virtiofs ...`, paired with Apple Virtualization XPC.
Settings normalize to EngineTypeMac=apple-virtualization-framework,
FileSharingTypeMac=virtiofs. No QEMU/TCG fallback in these tests.
Docker was installed from the official Intel DMG, verified signature; archive and
SHA256 are in assets/. Existing Docker app was absent and container directory4KiB.
The installer accepted the license; AutoStart=false, shared path limited to the
project's disposable Users directory. Initial settings backup is in disposable/.

The copied Podman profile and clean machines use isolated XDG config/data. The fresh
init does not reuse the diagnostic GRUB patch or the earlier provisioned clone.
Cached source image is read-only input. Global Docker socket alias still targets the
original Podman default machine; it is not the API used to validate either backend.
Harnesses explicitly select their Podman connection or actual Docker Unix socket.

Docker named volume proof survived normal engine restarts. The old clone's Podman
proof also survived the last host reboot. The NEW clean machine's repeated test read
FIXVM_NATIVE_PERSIST_OK before overwriting after restart, but the entire repeat was
canceled (exit143) because of severe latency; it is **not** another full-workload PASS.
Direct SSH verification timed out and is retained separately. Host reboot persistence
of the newly created clean Podman and Docker volumes remains pending.

## Failures and limits retained

- Original Podman Desktop auto-started a12-vCPU default VM while the isolated clean
  machine4CPU was provisioning. This concurrent run produced HPET timing warnings,
  CPU0 soft lockup and an explicit NMI request80000202 that37 rejects. The original
  start was canceled; Stop then HardStop was needed. Podman Desktop was quit.
  Original raw disk inode165146/size99857989632/mtime1793953935 still unchanged;
  do not infer that all original configuration/EFI-vars were untouched by auto-start.
- Interrupted first clean disk later failed Ignition useradd due to a leftover group
  lock. Both failed profile/disk and console logs are retained; fresh clean2 succeeded.
- Increasing clean2 to4CPU failed in EFI with compatibility-mode #GP/#PF, including
  RIPbfa55064/CR3bfc01000 and RIPbfa5505d/CR3zero, EFERd00, CSARc09b, bitmapzero.
  `0.3.37-clean2-podman-smp4-workload.log` exit125. Root cause is unresolved.
  Static CpuDxe AP code contains a matching-looking 32-bit far-jump sequence, but
  runtime bytes/target/GDT must be observed before attributing this fault.
- A later clean2/2CPU repeat was very slow; a Docker backend also reopened outside
  the harness during it. Docker was stopped (CLI timeout then TERM), then the slow
  Podman workload was explicitly canceled and guest shut down. No full repeat PASS.
  `0.3.37-clean2-podman-restart-strict.log`, host sample and console retained.
- Docker2 daemon ready about5.3s; Docker4 trials33.4–52.1s. Timing/clock warnings,
  global run serialization, concurrency and performance remain open. No causal
  conclusion that one lock/clock mechanism explains all these observations.
- Early Docker workload failed missing credential-helper PATH; second failed because
  Docker needs explicit `-f Containerfile`. Both fixed at harness level on same37.
- Early affinity subtests emitted `taskset: not found` and falsely printed a completion
  marker because shell wait ignored child failures. Those elapsed0.49/3.13s figures
  are invalid CPU-test results. Corrected strict test installs util-linux, waits each
  child successfully and checks all hashes. Its4.70s includes APK/network/container
  overhead; this is a smoke-workload duration, **not** a CPU performance benchmark.

Source changes to the Podman harness now also isolate runtime TMPDIR with a short,
private mktemp directory. XDG alone left gvproxy.pid in the user's shared runtime
directory; this was observed in the concurrent first test. Avoid Unix socket length
limits. After reboot, quit any auto-starting Podman Desktop/Docker UI before enabling
test gates and run one engine at a time. Do not mistake process interference for a
clean kernel regression. The new runtime isolation is syntax-checked; integration
with it is pending the next normal test cycle.

##38: bounded, capability-gated guest NMI path

Observed request: valid VM-entry info80000202 (type2/vector2), with IF irrelevant.
37 refused this known event.38 accepts it only in existing fast native code64,
with zero STI/MOV-SS/NMI blocking. On physical CPUs without BOTH CPUID8000000A.EDX
vGIF(bit16) and vNMI(bit25), a requested NMI/blocked state still returns unsupported.
No guest input becomes a host NMI, interrupt command or arbitrary host MSR write.

For capable native64 runs, VMCB VINTR enables vGIF(bit25), leaves guest GIF open
(bit9) and enables vNMI(bit26). Guest CLGI/STGI remain intercepted/unsupported;
physical host CLGI/STGI and IRQ restoration assembly are unchanged. Virtual NMI
blocking(bit12) maps to VMX interruptibility bit3, separately from interrupt shadow.
Hardware tracks NMI blocking on delivery and IRET. Explicit EVENTINJ stays one-shot,
including handled exits. NMI blocking does not itself mask IF-enabled external IRQs.
Step-mode software IRQ emulation rejects NMI-blocked contexts, preserving its scope.

No hidden V_NMI_PENDING queue, NMI-window primary control, VMX virtual-NMI pin control,
compatibility/real-mode NMI, guest debug/#MC, timing, scheduling or gate changes are
advertised. Existing unsupported controls remain rejected. The exception bitmap,
host physical-NMI intercept and original fault/XSTATE/speculation policies remain.
This does not claim to fix the EFI4 fault or the underlying watchdog/latency cause.

Primary references reviewed for register bits, vGIF dependency and direct-injection
blocking behavior: [AMD APM2](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2),
[Linux v6.12 SVM implementation](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kvm/svm/svm.c),
[Linux v6.12 SVM register definitions](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/svm.h).
Implementation uses the hardware model; no GPL source implementation was ported.

New signed nmiprobe covers IF0 delivery, one-shot entry, guest handler/counter,
blocking across API exit, rejection of nested blocked NMI without mutation, IRETQ
unblocking/stack/IF restoration, second NMI, external IRQ nested during NMI, and
architectural unblocking on nested IRQ's IRETQ. Added after existing fast gates.
ASan units validate allowed/rejected shadow combinations, malformed NMI, step/legacy
rejection and IRQ-vs-NMI blocking. New hardware cases remain pending loaded38.

On unsupported exits38 also logs vCPU id,15 instruction bytes and integer registers.
For decoded plain FF /5 [EDI] compatibility far jumps, it reads only owned guest
memory for target/selector/GDT descriptor through the existing bounded translation
planner, without committing A/D updates. No host pointer from guest data; no guest
repair/patch. This provides evidence for the unresolved EFI fault on the next run.

## Group review and activation

Fresh full70-PE inventory38 equals37; VZG11 SHA256 remains
3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903.
Reviewed CpuDxe AP16/32/64, IDT/IRET, NMI vs IRQ shadow and physical-NMI intercept,
vGIF dependency, event vectoring and one-shot lifetime, host state restoration,
owned-memory diagnostics and unsupported controls. Static PE inventory excludes
TE/raw/dynamic code, includes possible embedded data and proves no runtime reachability.

`runs/0.3.38-offline-checks.log`: build/sign/imports339, ASan/UBSan, all four FP/XSAVE
variants, resolver6 and all script syntax PASS, no compiler warning. Disassembly is
`runs/0.3.38-vcpu-svm-disassembly.txt`. Hardware38 is not established by these checks.

After reboot: confirm38 and idle gates; normal vCPU --modes → VM --rom → fast-test
(including NMI). Only after all pass, test one engine at a time: Docker4 strict,
clean Podman2 volume proof BEFORE writes, then EFI4 with new fault evidence. Capture
console timing/stalls and preserve any NMI guest diagnostic. Verify new volume data
after this host reboot. Product acceptance, default12CPU, long stability and meaningful
performance comparison remain open. Do not mark the project complete.

Installation verified: AuxKC38/expected path, installer exit0, kmutil exit28, valid signature, backup37 retained. Loaded37; all idle gates zero. See REBOOT-CHECKPOINT-0.3.38.md. No hardware38 success claim.
