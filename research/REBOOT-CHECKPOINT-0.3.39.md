# Reboot checkpoint: activate0.3.39

Hardware38: all three gates and NMI PASS; both backend named volumes survived host
reboot with exact read-only proofs before writes. Diagnostic Podman4 workload and
all four affinity hashes PASS with tsc=reliable. Argument removed, next default2
boot verified, proof unchanged, normal stop. Clean2 left2CPU/4096MiB/stopped.

39 samples high-frequency successful fast exits; full tracing can be enabled live
with `sudo sysctl debug.amdhv.trace_exits=1`, restore0 afterward. Exceptions,
unsupported exits and code/GPR/GDT diagnostics remain unconditional. No timer,
TSC, scheduler/lock, FP, NMI execution or assembly changes. New AP probe passes
192 real→PE→PAE/LME/PG→indirect far64 transitions on loaded38.

1. Confirm loaded39 and trace_exits=0; inspect latest PROCESS/PLAN and audit39.
2. Before enabling gates, stop any auto-started Docker/Podman Desktop VM. Check
   vfkit/com.docker.virtualization process paths, vms=0. One engine at a time;
   Docker unexpectedly reopened during a prior isolated test despite AutoStartfalse.
3. Run vcpu-test.sh --modes, vm-test.sh --rom, fast-test.sh. Fast gate tests the
   new trace sysctl live and new aplongprobe. Any failure: retain diagnostics,
   restore runtime gates, investigate before container integration.
4. Use existing exact read-only proof scripts for both backends before writes.
   Podman profile `research/disposable/podman-profile`, machine fixvm-clean2,
   short private TMPDIR through native-podman-probe.sh. Docker harness uses actual
   ~/.docker/run/docker.sock, not /var/run/docker.sock (original Podman alias).
5. Compare startup/workload/clock behavior with sampled logging first. Increase
   clean2 to4CPU only while stopped. Keep default guest kernel arguments first;
   record actual /proc/cmdline and clocksource/dmesg. No concurrent Docker VM.
   Default4 acceptance still pending; diagnostic4 PASS is not default acceptance.
6. If reproducing EFI GP/PF, enable full exit trace live only as needed; preserve
   unconditionally logged id/code/GPR/far pointer/GDT. The isolated AP probe is
   coverage, not proof that the actual shared firmware exchange is correct.
7. Compare trace on/off on the SAME39 if useful; capture workload and start times,
   CPU counts, current clocksource, concurrent processes, and normal teardown.
   No extra host reboot is needed just to switch trace or restart a guest.

Restore clean2 to2CPU if4 remains unreliable; stop all test engines and restore
takeover/fast/trace_exits to0. Full goal remains open: default SMP stability,
clock correctness, repeated tests and realistic performance/concurrency.

Installation/backup results: see runs/0.3.39-install.log and latest PROCESS entry.
No automatic reboot.
