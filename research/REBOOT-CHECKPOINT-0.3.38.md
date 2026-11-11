# Reboot checkpoint: activate0.3.38

Installed/AuxKC38 verified, loaded37, backup37 retained. All gates zero, no VM/provider
left running. Installer exit0; kmutil exit28. See `runs/0.3.38-install.log`.
This is a hardware activation boundary, not project completion.

1. Confirm loaded38. Read PROCESS/PLAN latest and NATIVE-NMI-AUDIT-0.3.38.md.
2. Quit any auto-started Podman Desktop or Docker backend before enabling takeover.
   Original Podman default is still12CPU and must not interfere with isolated tests.
3. vcpu-test.sh --modes → vm-test.sh --rom → fast-test.sh. New NMI probe is last.
   If any fails, stop integration and preserve full diagnostics; restore gates.
4. One engine at a time. Fresh Podman profile:
   `/Users/phucduong/Desktop/FixVM/research/disposable/podman-profile`, machine
   `fixvm-clean2` (2CPU/4096MiB/disk16GiB). native-podman-probe.sh now uses a new
   private short TMPDIR each run; metadata, image and SSH keys remain persistent.
5. Before full workloads overwrite proofs, use `disposable/verify-reboot-volume-0.3.38.zsh`
   through `disposable/verify-podman-reboot-0.3.38.zsh` as the native harness
   workload argument. It inspects existing
   volume first, runs --pull=never/read-only, and checks exact proof. No auto-create.
6. Docker is installed at /Applications/Docker.app,4.99.0/build245822; settings4CPU/
   4096MiB/AppleVZ/virtiofs/AutoStartfalse. Actual socket is
   `/Users/phucduong/.docker/run/docker.sock`; ignore /var/run/docker.sock (Podmanalias).
   docker-native-probe.sh enables/restores gates and stops backend. Use read-only proof
   wrapper `disposable/verify-docker-reboot-0.3.38.zsh`, then strict workload if proof passes.
7. Capture Podman4EFI fault15-byte instruction/GPR/far-pointer/GDT evidence on38.
   The fault is not claimed fixed. Do not call all SMP/stability/performance complete.

Known37 passes: normal gates669/37/255; directLinuxSMP2; Podman cloneSMP2 and fresh
clean2SMP2 full workload; Docker2 and strictDocker4 full workload with all four
CPU-affinity32MiB hashes. Docker volume survives VM restarts. Clean2 repeat read
prior proof but full repeat canceled143 due latency/extraDocker startup. Full host
reboot proof of the NEW Podman/Docker volumes remains pending.

NMI38: explicit native64 injection only, BOTHvGIF16/vNMI25 capabilities, private
hardware blocking, no pending queue/window controls. New probe covers IF0, repeated
NMI, blocking, IRET and nested IRQ. Offline PASS; hardware38 pending this reboot.
