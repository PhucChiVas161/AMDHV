# Native backend test results — 0.3.40

2026-11-11. Loaded AMDHV0.3.40 on Ryzen9900X, macOS25G332. These are bounded
hardware/application results, not full project completion or long-term stability.

## Hardware

Before and again after the user's hard restart, all gates exited0 with zeroFAIL:
vCPU modes669 / VM ROM37 / fast serial273 / fast parallel273 PASS lines. NMI,
CPL3, extended FP, cancellation, AP real->compatibility->64 and mapping/lifetime
regressions PASS. Parallel guest handshake64/64; serial baseline0/64.
`runs/0.3.40-{vcpu-test,vm-test,fast-serial,fast-parallel}.log` and corresponding
`0.3.40-recovery-*` logs. Physical selftest PASS24/24 before the hard restart.

Two independent VM tasks also ran simultaneously: peak2VMs; SMP handshake63/64,
AP slots1..3 each64/64 transitions, data-page writer progress and teardown PASS,
exit0. Distinct guest code/page tables exercise simultaneous ASID/NPT use across
tasks. `runs/0.3.40-multivm.log` and its smp/ap child logs.

## Docker Desktop4.99.0 / Apple Virtualization /4CPU /4GiB

Actual com.docker.virtualization/Apple VZ backend, CLI/engine29.9.3, Linux7.0.14.
No QEMU fallback. All successful full runs include exact readonly prior-volume
proof before writes, container/DNS/bind/volume/uncached build/API/published port,
all CPU0..3 affinity hashes, and teardown to0VMs.

| Run suffix in runs/0.3.40-docker-*.log | Mode | Engine ready | Full workload |
|---|---|---:|---:|
| serial | serial |8.427s|15.40s|
| parallel | parallel |1.898s|4.33s|
| parallel-repeat2-stress | parallel |1.875s|5.31s|
| parallel-repeat3-retry | parallel |1.883s|5.80s|
| parallel-repeat4 | parallel |1.892s|5.35s|
| parallel-recovery | parallel, after hard reset |2.399s|5.40s|
| serial-warm-resume | serial, warm comparison |8.401s|13.97s|
| parallel-final | parallel, warm comparison |1.853s|6.02s|

Engine ready times are Docker's reported backend durations; full workload times
exclude startup. This is one machine/image/configuration and a small sample, not
a general benchmark. Warm comparison supports a substantial startup improvement
with parallel execution on this configuration. Package/network/container overhead
remains inside full-workload and nested affinity timings.

Extra stress: four concurrent cached-image containers, each pinned to a different
guest CPU, memory limit256MiB each, write/sync32MiB then64 verified SHA256 reads.
All256 hashes correct,8GiB aggregate reads, exact volume proof unchanged.6.62s
includes container/API/teardown overhead; not pure CPU or uncached disk throughput.
`runs/0.3.40-docker-parallel-repeat2-stress.log`.

Original repeat3 never launched a VM because the previous backend still owned its
single-instance socket; canceled143, NOT a workload/kernel failure or PASS.
Harness now rejects an already-running backend and waits for monitor/services
exit after VM teardown. Repeat3-retry and repeat4 both pass with shutdown marker.
Some headless CLI stops time out before the monitor exits; existing owned-launcher
TERM plus process-drain completes teardown. No forced guest disk stop in these
successful runs. Retain canceled `parallel-repeat3.log` separately.

## Podman5.8.9 / applehv-vfkit /CoreOS44 /Linux7.2.7

Isolated profile research/disposable/podman-profile, clean2 machine with original
guest boot path/default kernel arguments: **no tsc=reliable diagnostic argument**.
All entries below pass the same full workload, exact prior-volume proof, affinity
hashes and normal provider stop; current clocksource was tsc at workload entry.

| Run suffix in runs/0.3.40-podman-*.log | CPU/RAM | Provider start | Full workload |
|---|---|---:|---:|
| parallel-smp2 |2CPU/4GiB|31.54s|7.02s|
| parallel-smp4 |4CPU/4GiB|30.47s|7.21s|
| parallel-smp4-repeat2 |4CPU/4GiB|30.49s|5.69s|
| parallel-smp4-repeat3 |4CPU/4GiB|30.45s|6.22s|
| parallel-smp4-recovery |4CPU/4GiB, after hard reset|31.08s|6.90s|

Three consecutive4CPU cycles pass before interruption; recovery adds a fourth.
Console logs retained. No collected TSC-unstable fallback/soft-lockup/Oops pattern
in these consoles; this does not prove long-term clock correctness. Earlier EFI
fault not reproduced in this sample; its original root cause remains unproven.

Provider start is not Linux boot time. All isolated starts warn API socket ping
failed: global /var/run/docker.sock points through the original user's Podman
alias, while the isolated machine forwards its own short-TMPDIR socket. Upstream
v5.8.9 WaitAPIAndPrintInfo calls WaitAndPingAPI even with --no-info; config.go uses
20s timeout. Isolated API success is checked separately by the workload. This
explains an additional test-profile socket wait, not all historical latency.
Sources retained in external/podman-v5.8.9; primary references:
https://github.com/containers/podman/blob/v5.8.9/pkg/machine/config.go and
https://github.com/containers/podman/blob/v5.8.9/pkg/machine/machine_common.go .
The global helper/alias was not reconfigured for this experiment.

## Power interruption and recovery

Host auto-entered Idle Sleep at12:10:22 during `docker-serial-warm.log`; user
reported wake stuck and hard restarted (new boot13:17). That workload is
interrupted, not PASS; planned next steps never executed. See
SLEEP-WAKE-INCIDENT-0.3.40.md and retained power logs. No attribution to kext or
parallel execution is established; the interrupted run used parallel0.

Resumed gates and exact readonly data proofs for BOTH backends pass after hard
reset, followed by full workloads and the warm comparison. Probe lifetimes now
hold a caffeinate idle-sleep assertion. Persistent power settings remain unchanged.
This is mitigation for testing, not a sleep/wake fix. No additional reboot/kext
version was needed to finish the interrupted tests.

## User handoff and remaining scope

Docker and Podman test engines stopped. clean2 left stopped4CPU/4GiB; original
12CPU Podman profile not part of qualification. Docker settings4CPU/4GiB/AppleVZ/
virtiofs/AutoStartfalse, with the disposable Users share. CLI contextdesktop-linux.
After tests the helper intentionally enables fast1/parallel1/takeover1 with0VMs,
trace0/hv_support1 so the user can open Docker Desktop. This active backend is
intentional handoff state, not an orphan VM. See runs/0.3.40-desktop-ready.log.

User-level temporary idle-sleep guard2664 started13:28:36 for7200s (until about
15:28 local, unless stopped earlier). It does not persist across reboot. See
USER-DOCKER-0.3.40.md for manual activation/stop/context/share/power instructions.

Remaining before full project completion: host sleep/wake, sustained long-term
workloads, broader configurations including original Podman12CPU, clock validation,
normal daily-use activation and provider socket integration. Do not mark the
whole goal complete from these passing bounded tests.

Latest handoff update: a normal Docker backend started13:29:29. API ping OK,
engine29.9.3/4CPU/Desktop; cached ephemeral smoke container exited0 with x86_64,
4,FIXVM_DOCKER_GUI_OK. Current1VM is intentionally left running for the user's
trial. User explicitly defers sleep/wake testing and prioritizes working VMs.
