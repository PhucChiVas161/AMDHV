# Reboot checkpoint: activate0.3.40

**Superseded by completed activation/testing on2026-11-11.** See
NATIVE-ACCEPTANCE-0.3.40.md and USER-DOCKER-0.3.40.md. Hardware and application
tests pass; sleep/wake interruption/mitigation recorded separately. Final handoff
intentionally leaves fast/parallel/takeover1 with0VMs for the user's Docker trial.
The original activation instructions below are historical context.

Loaded39 hardware gates669/37/271 PASS. Docker4 complete workload and Podman4
DEFAULT guest arguments complete workload/all CPU hashes PASS; both exact named
volume proofs survived host reboot before writes. Podman4 repeat canceled before
engine readiness; not stable. Clean2 restored2CPU/4GiB/stopped. Docker4CPU/4GiB.
All test engines stopped, takeover/fast/trace/vms/hv_support0 before installation.

40 adds opt-in parallel fast run leases; defaultparallel0 keeps runs exclusive.
Both provider harnesses now require the40 sysctl and explicitly select0 unless
FIXVM_PARALLEL=1; cleanup restores the previous flag. Source/build40 offline PASS,
hardware40 not tested. Broad audit: PARALLEL-RUN-AUDIT-0.3.40.md.

1. Confirm loaded40 and all gates0, including debug.amdhv.parallel. Stop any
   auto-opened Docker/Podman Desktop engine; inspect vfkit disk paths/processes.
2. Run scripts/vcpu-test.sh --modes, scripts/vm-test.sh --rom, scripts/fast-test.sh
   in SERIAL mode first. New smpprobe baseline expects0/64 first-call overlaps,
   no errors; active-guest mapping writer/teardown phase must pass.
3. Run scripts/fast-test.sh --parallel. It configures parallel1 before takeover,
   refuses policy changes while active, tests host IRQ bounds first, then smpprobe
   requires positive first-call overlap plus mapping writer/teardown PASS. Full
   AP transitions/FP/NMI/CPL regressions run too. Cleanup restores all flags.
   Any failure: retain diagnostics and use parallel0; investigate, do not proceed
   to providers with parallel1 merely because serial mode passed.
4. Read-only exact volume proof BEFORE writes using existing wrappers. Podman
   isolated profile research/disposable/podman-profile, machine fixvm-clean2;
   first2CPU baseline, then set4 only while stopped. No tsc=reliable diagnostic
   argument should be active. verify-podman-0.3.39.zsh asserts this, logs clocks,
   reads exact prior proof and runs the full workload/strict affinity test.
5. With parallel hardware gate passing, explicit experiment:

   sudo env FIXVM_PARALLEL=1 scripts/native-podman-probe.sh "$PWD/research/disposable/podman-profile" fixvm-clean2 "$PWD/research/disposable/verify-podman-0.3.39.zsh"

   sudo env FIXVM_PARALLEL=1 scripts/docker-native-probe.sh "$PWD/research/disposable/verify-docker-0.3.39.zsh"

   Run separately and wait for actual exit/gates0 between them. Docker socket is
   ~/.docker/run/docker.sock; /var/run/docker.sock still aliases original Podman.
6. Capture Podman provider startup time (new external time scope), current clock,
   cmdline/watchdog warnings, all four hashes, exact persistence and normal stop.
   Repeat to assess stability. Docker baseline39 engine ready16.436s, fullworkload
   23.47s; Podman39 fullworkload40.37s excludes startup. Nested affinity durations
   include package/container/network overhead; not CPU benchmarks.
7. Compare serial/parallel on the SAME40 with only guest restarts. No host reboot
   needed to toggle parallel when takeover0/vms0. Full trace can toggle live via
   trace_exits; default sampled logging retained. Save unconditional EFI fault
   code/GPR/far-pointer/GDT evidence if it recurs; no fault root-cause claim yet.
8. If4 unreliable restore clean2 to2CPU. End with all test engines stopped and
   runtime gates/parallel0. Do not mark full goal complete from a single pass.

Installation and backup39 details: runs/0.3.40-install.log / latest PROCESS.
No automatic reboot. Hardware parallel speed/stability remains pending.
