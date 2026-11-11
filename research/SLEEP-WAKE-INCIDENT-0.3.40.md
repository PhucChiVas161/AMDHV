# Sleep/wake interruption — 2026-11-11

User reported host stuck on sleep/wake and hard restarted. Current boot began
13:17:16 local. Loaded40, all runtime flags/VMs initially0, no running engine.
Podman Desktop UI auto-opened and was quit before testing resumed.

## Evidence

- pmset records **Idle Sleep at12:10:22 +0700**, while the serial warm Docker
  comparison was executing. Original power configuration has sleep10min on AC.
- Last `0.3.40-docker-serial-warm.log` entries reach12:10:23, before the workload
  finishes. Treat that attempt as interrupted, not a PASS or timing measurement.
  Its planned next parallel run and desktop-ready activation never executed.
- No completed wake is visible between that sleep and the next boot in the
  collected power log. User report establishes the failed wake experience;
  available logs do not establish which component caused it.
- No newly matching panic/sleep-wake-failure report was found in the enumerated
  system/user DiagnosticReports directories. Absence of a saved report does not
  exclude a kernel/power/graphics failure. Unified log buffering may be incomplete
  after hard reset. Prior shutdownStall files predate this incident.
- The interrupted comparison used **parallel0**, takeover1/fast1, with a Docker
  guest. Therefore this incident alone cannot be attributed to parallel runs.
- Saved `runs/0.3.40-hardrestart-pmset.log` and
  `runs/0.3.40-hardrestart-unified-power.log`.

## Mitigation and resumed testing

Use a temporary caffeinate idle-sleep assertion during testing. Provider probes
now launch `caffeinate -i -w $$`, automatically releasing the assertion when that
probe exits. This includes teardown and does not change persistent pmset settings.
Assertions were observed in pmset during a running recovery probe and the previous
probe's assertion was gone after its exit. A separate user-level7200s assertion
also covers the manual Docker trial window; see current run/PROCESS status.

This prevents automatic idle sleep during the trial. It does not repair suspend/
resume, block all manual sleep requests, or qualify sleep/wake support. No kernel
patch/version/reboot was introduced for this mitigation.

After hard reset: repeat modes/VM/fast serial+parallel gates, read exact existing
volume proofs before writes for both engines, repeat full native workloads,
complete the interrupted comparison, and only then enable the backend for the
user's manual GUI trial. Results belong in NATIVE-ACCEPTANCE-0.3.40.md.
