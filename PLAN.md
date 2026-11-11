# FixVM: Hypervisor.framework cho Ryzentosh (AMD SVM)

Mục tiêu cuối cùng: **Podman và Docker Desktop sử dụng được backend ảo hóa native
của macOS trên máy AMD Ryzen**, thông qua AMDHV tương thích API/ABI của Apple.
Ưu tiên Podman `applehv`/vfkit (Virtualization.framework), sau đó Docker Desktop với
backend Apple phù hợp phiên bản macOS/x86_64 đã cài. Xác minh backend thực tế trước khi
kết luận tương thích; đây là mục tiêu phát triển, chưa phải tính năng đã đạt.

“Native macOS” ở đây là ứng dụng/CLI và backend ảo hóa chạy trên macOS; Linux container
vẫn chạy trong máy ảo Linux do backend quản lý. Đường sử dụng cuối cùng không phụ thuộc
QEMU/Lima, không dùng TCG và không chuyển workload sang máy Linux từ xa.
QEMU `-accel hvf` chỉ là công cụ kiểm chứng trung gian cho Hypervisor.framework;
boot Linux bằng QEMU chưa đủ để hoàn thành dự án. `kern.hv_support = 1` cũng chỉ là
điều kiện ban đầu, không phải tiêu chí thành công.

Phát triển và thử trực tiếp trên máy thật. Giữ nguyên framework và ứng dụng nếu khả thi;
ưu tiên hoàn thiện AMDHV để các backend Apple hoạt động đúng.

## Tiêu chí hoàn thành dự án

- Podman dùng provider `applehv`/vfkit: tạo/start/stop/restart máy Linux và chạy container
  thành công; xác nhận provider/process thực tế, không fallback sang QEMU/TCG.
- Docker Desktop dùng backend native macOS được phiên bản cài đặt hỗ trợ: engine khởi động,
  chạy container và build image thành công; ghi rõ phiên bản và backend đã kiểm chứng.
- Cả hai đạt kiểm thử network/DNS/published ports, bind mount và volume bền vững,
  build image, workload nhiều vCPU; dữ liệu còn nguyên sau restart VM và reboot host.
- Có log kiểm thử lặp lại, không kernel panic/treo/rò VM sau teardown; đo hiệu năng workload
  thực tế. Đường chạy sử dụng thường ngày không giữ cơ chế single-step phục vụ debug.
- Chỉ đánh dấu hoàn tất khi cả Podman và Docker Desktop đạt các tiêu chí trên. Nếu có giới hạn
  tương thích, ghi rõ phần còn thiếu; không thay mục tiêu bằng QEMU boot Linux.

## Kết quả gần nhất (2026-11-11)

AMDHV0.3.40 đã qua gates669/37/273/273 ở cả tuần tự/song song, lặp lại sau hard
restart đều đạt. Docker4CPU/4GiB AppleVZ đã qua nhiều lượt full workload, hash4CPU,
volume sau reboot/hard reset và stress4container/8GiB dữ liệu đọc. Cặp đo warm:
engine ready8.401s tuần tự,1.853s song song. Podman applehv4CPU/4GiB với tham số
guest mặc định qua3chu kỳ liên tiếp và lượt recovery, đầy đủ workload/hash/volume.
Backend fast1/parallel1/takeover1, trace0. Docker đã được người dùng dùng thành công;
hiện Docker dừng và Podman mặc định đang chạy1VM theo yêu cầu mới. Đã sửa metadata
khởi tạo Ignition và file registry bị thiếu của máy mặc định; nguyên12CPU/3814MiB,
2 lượt full workload đạt, gồm lượt sau stop/start và volume proof chính xác.
Chi tiết: [PODMAN-DEFAULT-0.3.40.md](research/PODMAN-DEFAULT-0.3.40.md).
Sleep/wake được hoãn theo yêu cầu người dùng; dùng giữ thức tạm khi thử VM.
Đây là kết quả kiểm thử có giới hạn; chưa hoàn thành mục tiêu dài hạn/mọi cấu hình.
Chi tiết: [NATIVE-ACCEPTANCE-0.3.40.md](research/NATIVE-ACCEPTANCE-0.3.40.md),
[USER-DOCKER-0.3.40.md](research/USER-DOCKER-0.3.40.md).

## Hiện trạng (2026-11-06)

- CPU: Ryzen 9 9900X (Zen 5), 24 luồng, 64 GB RAM. SVM đã bật trong BIOS.
- macOS 26.7.2 (25G332), XNU 12377.161.15, x86_64.
- KDK 25G332 đã cài: `/Library/Developer/KDKs/KDK_26.7.2_25G332.kdk`.
- `kern.hv_support = 0`. `com.apple.driver.AppleHV` có nạp nhưng chỉ hỗ trợ Intel VMX.
- SIP: Kext Signing và FS Protections đã tắt. boot-args có `keepsyms=1 debug=0x100`.
- OpenCore ở disk0s5 (chung EFI với Limine). Có patch AMD Vanilla, `SecureBootModel = Disabled`.

## Kết quả reverse engineering

### Phía kernel (XNU), do kernel export
`hv_set_traps`, `hv_release_traps`, `hv_set_callbacks`, `hv_release_callbacks`,
`hv_set_task_target`, `hv_set_thread_target`, `hv_get_*_target`, `hv_ept_pmap_create`,
`hv_io_notifier_grp_*`, `hv_ast_pending`, `hv_get_volatile_state`.
Biến `hv_support_available` (đứng sau sysctl `kern.hv_support`) được `hv_support_init`
gán từ `vmx_hv_support()`, nên trên AMD luôn bằng 0.

### Phía userspace
Hypervisor.framework không dùng IOKit: nó gọi `hv_task_trap(index, arg)` và
`hv_thread_trap(index, arg)` qua machdep trap. Ngoài ra còn có vùng nhớ chia sẻ
`hv_vmx_vcpu_rw_interface_t` / `ro_interface_t` (cache trường VMCS) và hv commpage.

### Bảng trap (14 + 14), lấy từ AppleHV 25G332

| # | task trap (theo VM) | thread trap (theo vCPU) |
|---|---|---|
| 0 | vm_create | vcpu_run |
| 1 | vm_destroy | vcpu_run_until |
| 2 | vm_addrspace_create | vcpu_invalidate_tlb |
| 3 | vm_addrspace_destroy | vcpu_enable_managed_msr |
| 4 | vm_map | vcpu_disable_managed_msr |
| 5 | vm_unmap | vcpu_destroy |
| 6 | vm_protect | vcpu_set_addrspace |
| 7 | vm_sync_tsc | vmx_vcpu_read_vmcs |
| 8 | vm_add_io_notifier | vmx_vcpu_set_shadow_access |
| 9 | vm_rem_io_notifier | vmx_vcpu_set_apic_address |
| 10 | vm_request (APIC/IOAPIC/PIC) | vmx_vcpu_set_msr_access |
| 11 | vm_intr_msi | vmx_vcpu_read_drs |
| 12 | vcpu_create | vmx_vcpu_get_cap_write_vmcs |
| 13 | vcpu_interrupt | vmx_vcpu_request (APIC theo vCPU) |

### Cấu trúc AppleHV
- Tầng chung: `hv_vm_t`, `hv_vcpu_t`, `hv_vmx_vma_t`, `hv_vlapic_t`, `hv_vioapic_t`, `hv_vatpic_t`.
  Tên hàm (`vatpic_icw1`, `vioapic_process_eoi`, ...) trùng với **bhyve** của FreeBSD.
- Tầng Intel: `hv_vmx_vcpu_t`, `hv_vmx_vcpua_t`, `hv_vmx_space_t` (EPT).
- Có khoảng 200 lệnh VMX inline trong 55 hàm, nên **không thể** giữ AppleHV rồi chỉ
  vá các lệnh VMX (mỗi lệnh bị #UD trap, quá chậm).

## Kiến trúc đã chọn: kext viết mới (clean-room) cùng ABI

```
Podman / Docker / UTM / QEMU-hvf          (không sửa)
Hypervisor.framework                       (không sửa)
        │  hv_task_trap / hv_thread_trap, vùng nhớ chia sẻ
AMDHV.kext
  ├─ trap table giống hệt AppleHV (14 + 14)
  ├─ lớp tương thích VMX: VMCS ảo ⇄ VMCB, SVM exitcode → VMX exit reason
  ├─ lõi SVM: VMRUN, NPT, ASID, xử lý intercept (dựa trên bhyve amd/svm.c, BSD)
  └─ vlapic / vioapic / vatpic (port từ bhyve, BSD)
AMD-V (SVM, NPT, AVIC)
```

Cách cài: chặn AppleHV (`Kernel.Block` trong OC, hoặc để nó không đăng ký trap trên AMD),
AMDHV đăng ký trap bằng `hv_set_traps` và gán `hv_support_available = 1`.

## An toàn khi thử trên máy thật

- Tahoe không cho nạp kext bên thứ ba ngay: `kmutil load` chỉ gộp kext vào
  Auxiliary Kernel Collection, và kext có hiệu lực sau khi reboot, rồi **tự nạp mỗi lần boot**.
  Mỗi lần sửa kext = install + reboot.
- Vì vậy lúc boot AMDHV chỉ đọc CPUID (vô hại). Mọi thứ đụng tới SVM chỉ chạy khi
  được gọi qua `sysctl debug.amdhv.*`, nên nếu panic thì reboot là máy về trạng thái an toàn.
- Công tắc tắt khẩn cấp: boot-arg `-amdhvoff` (thêm tạm ở OpenCore picker hoặc
  config.plist), khi đó AMDHV không start.
- Chỉ đưa vào `Kernel.Add` của OC khi đã ổn định. Trước khi đổi config luôn backup.
- Panic log: `/Library/Logs/DiagnosticReports/*.panic`, có symbol nhờ `keepsyms=1`.
- Có USB/EFI dự phòng (người dùng đã chuẩn bị).

## Cấu trúc dự án

```
Makefile                 build bằng Command Line Tools: `make`
AMDHV/Info.plist         bundle com.fixvm.driver.AMDHV, match IOResources
AMDHV/src/svm_defs.hpp   MSR, offset VMCB, exit code, helper rdmsr/cpuid
AMDHV/src/svm_vmrun.S    trampoline clgi → vmrun → lưu GPR guest → stgi
AMDHV/src/svm_selftest.* chạy guest thử trên mọi nhân (mp_rendezvous_no_intrs)
AMDHV/src/AMDHV.cpp      IOService: probe, self-test, ghi kết quả vào IORegistry
scripts/install.sh       sudo: cài vào /Library/Extensions, rebuild AuxKC (cần reboot)
scripts/selftest.sh      sudo: sysctl debug.amdhv.selftest=1 (chạy VMRUN trên mọi nhân)
scripts/uninstall.sh     sudo: gỡ kext, kmutil rebuild (cần reboot)
scripts/trap-trace.sh    sudo: takeover trap → chạy hvprobe → in log → gỡ trap AMDHV
scripts/status.sh        xem sysctl, IORegistry và log của AMDHV
AMDHV/src/kernel_resolver.* tìm hàm hv_* private: slide từ IOLog, kiểm tra version + byte đầu hàm
AMDHV/src/kernsyms.h     sinh bằng `make kernsyms` từ KDK + Boot KC cùng UUID (làm lại sau update macOS/rebuild Boot KC)
AMDHV/src/hv_traps.*     bảng trap 14+14 (hiện là stub: ghi log, trả HV_UNSUPPORTED)
AMDHV/src/klog.*         ring log 64 KB, đọc bằng `sysctl -n debug.amdhv.log`
tools/hvprobe/           tool gọi Hypervisor.framework (có entitlement hypervisor)
tools/check_imports.sh   `make check`: mọi symbol import đều được KPI export
```

## Các giai đoạn

1. **Khung kext + SVM tối thiểu** (XONG 2026-11-06: PASS 24/24, exits CPUID→HLT, guest RAX đúng): kiểm tra CPUID 0x8000000A, VM_CR.SVMDIS,
   bật EFER.SVME trên từng nhân, cấp phát host save area, VMRUN một guest
   vài lệnh rồi `hlt`. Gỡ kext sạch.
2. **Đăng ký trap** (XONG: 0.2.2 takeover + trace vm_create + cleanup PASS): xem AppleHV đã giữ trap chưa (trên AMD), đăng ký bảng trap của
   AMDHV, bật `kern.hv_support`. Kiểm tra bằng một tool nhỏ gọi `hv_vm_create`.
3. **Nhóm API cơ bản**: vm_map/unmap/protect (NPT), vcpu_create/run, read/write
   register, VMCS ảo, exit reason, mode transitions và FPU. Mốc kỹ thuật trung gian:
   QEMU `-accel hvf` boot Linux; chưa phải hoàn thành mục tiêu sản phẩm.
4. **Backend Apple và Podman native macOS**: hoàn thiện API mà Virtualization.framework/
   vfkit thực sự cần, gồm vm_request/vcpu_request, APIC/IOAPIC/PIC, MSI, run_until,
   io_notifier theo kết quả trace. Mốc: Podman provider `applehv` chạy container, build,
   network, mount và lifecycle ổn định. QEMU/Lima không phải bước triển khai bắt buộc.
5. **Docker Desktop native macOS và độ ổn định**: xác minh backend/phiên bản trên x86_64,
   hoàn thiện phần tương thích còn thiếu, đạt bộ tiêu chí cho cả Docker và Podman;
   kiểm thử reboot, workload dài và hiệu năng trước khi kết luận dùng hằng ngày.

## Kết quả giai đoạn 1 (máy thật)

- SVM rev 1, 32768 ASID, VM_CR = 0x8 (BIOS lock, SVM bật).
- Tính năng: NPT, LbrVirt, SVML, NRIPS, TscRate, VmcbClean, FlushByAsid, DecodeAssists,
  PauseFilter/Threshold, AVIC, x2AVIC, VirtVmsaveVmload, vGIF, GMET, vNMI.
- Self-test VMRUN chạy ổn trên cả 24 nhân, EFER được khôi phục sau test.

## Chuẩn bị giai đoạn 2

- 0.2.2 đã chạy trên máy thật: resolver PASS, takeover PASS, hvprobe tới task[0] vm_create,
  trả HV_UNSUPPORTED đúng stub; cleanup khôi phục takeover=0/hv_support=0.
- Log kiểm chứng: `research/runs/0.2.2-phase2-verification.log`.
- ABI giai đoạn 3: `research/PHASE3-ABI.md`; chưa triển khai VM thật.
- 0.2.1 bị chặn vì byte RIP-relative trong KDK khác Boot KC sau liên kết. 0.2.2 dùng
  symbol KDK + UUID khớp kernel trong KC, lấy chữ ký đủ 24 byte từ KC; không bỏ kiểm tra.
- Cleanup hiện chỉ gỡ trap AMDHV và khôi phục hv_support; trap AppleHV được đăng ký lại sau reboot.

- `AppleHV::start` **không kiểm tra CPU**: luôn gọi `enable_callbacks` + `enable_traps`.
  Vì vậy trên AMD, AppleHV đang giữ trap. Hướng xử lý: AMDHV gọi `hv_release_traps` /
  `hv_release_callbacks` rồi đăng ký của mình (không cần chặn AppleHV trong OC). Phương án
  dự phòng: `Kernel.Block` AppleHV.
- `kern.hv_support` là `SYSCTL_INT` trỏ vào `hv_support_available`. Tìm được địa chỉ biến
  bằng cách duyệt `sysctl__kern_children` (đã export) và lấy `oid_arg1`, không cần resolve symbol private.
- `hv_set_traps` và họ hàng nằm trong `com.apple.kpi.private`: cần thử xem kmutil có cho
  kext bên thứ ba link vào đó không. Nếu không, resolve lúc chạy.
- Disassembly AppleHV lưu ở `research/AppleHV-25G332.s`.

## Quan sát từ hvprobe (khi AppleHV còn giữ trap, 2026-11-06)

- `hv_vmx_read_capability` trả HV_SUCCESS, giá trị 0, **không đi qua trap**: framework đọc
  capability VMX từ hv commpage (`AppleHV::map_hv_commpage`). Giai đoạn 3 cần dựng commpage này.
- `hv_vm_create` → HV_ERROR (do AppleHV không có VMX).
- Gọi `hv_vmx_vcpu_read_vmcs` trên vCPU chưa tạo sẽ làm process crash: framework không tự kiểm tra.

## Câu hỏi còn mở

- Layout của `rw_interface` / `ro_interface` và hv commpage. Cần cho giai đoạn 3.
- Virtualization.framework có kiểm tra gì thêm ngoài `kern.hv_support` (model CPU, MSR VMX)?

## Cập nhật phase 3: 0.3.0 / 0.3.1

- 0.3.0 đang nạp: VM lifecycle, mapping RW/RO và NPT map/unmap/protect đã PASS máy thật.
- 0.3.1 đã build và cài AuxKC, chờ reboot: vCPU theo thread, VMCS ảo/cache, VMSAVE/VMLOAD host và đường SVM real-mode
  giới hạn 1024 bước. Chưa runtime-test; cần reboot và chạy `scripts/vcpu-test.sh`.
- Chưa xong phase 3: thiếu protected/long mode, FPU, VMX capabilities/controls đầy đủ,
  exit emulation và interruption để QEMU HVF boot Linux.
- Giới hạn memory hiện tại: space 0, trang 4 KiB, 1 GiB pinned/VM, 1024 bảng NPT, 16 VM;
  không hỗ trợ quyền execute-only/write-only do NPT không biểu diễn được.
- Log VM/NPT: `research/runs/0.3.0-vm-test.log`. Unit tests: `make test-memory test-vcpu`.

## Cập nhật sau reboot 0.3.1

- 0.3.1 đã PASS VM/NPT và vCPU lifecycle/SVM real mode trên máy thật; logs tại
  `research/runs/0.3.1-{vm,vcpu}-test.log`. Cleanup sạch.
- 0.3.2 thêm protected32 và long64 presets, chuyển segment attributes, test guest page walk.
  Build/imports và test-modes/test-vcpu/test-memory PASS; AuxKC đã xác nhận 0.3.2, chưa runtime-test.
- Lần reboot tiếp theo: xác nhận 0.3.2, chạy VM regression và `scripts/vcpu-test.sh --modes`.
- Phase 3 vẫn chưa hoàn tất: presets chưa đủ boot Linux; còn mode transitions, FPU,
  capability/controls, exit emulation và interrupt handling tương thích QEMU HVF.

## Native backend: capability gate đã kiểm chứng

- Podman 5.8.9 + vfkit 0.6.4 có sẵn; machine hiện hữu dùng applehv và đang stopped.
  Chưa cài Docker Desktop; chưa đạt tiêu chí runtime container của cả hai.
- `make vzprobe` / `sudo scripts/vz-probe.sh`: trên 0.3.1, takeover làm
  Virtualization.framework.isSupported chuyển false → true, validate EFI VM 1 CPU/512 MiB PASS.
  Tool dùng EFI store tạm rồi xóa, không start VM. Chưa được coi là backend đã chạy Linux.
- Tiếp theo vẫn là reboot 0.3.2 và mode runtime tests; giữ nguyên machine/dữ liệu Podman hiện hữu.

## Checkpoint mới nhất: 0.3.3 (thay bản staged 0.3.2)

- Native EFI start trên 0.3.1 đã tới vm_create/map RAM/map ROM, chưa tới vCPU; start FAIL.
- Host read-only → guest RX mapping lỗi đã được tái hiện độc lập. 0.3.3 sửa pin direction,
  ngăn nâng write permission trên read-only pin và bổ sung trap return-code logs.
- 0.3.3 chứa cả mode presets của 0.3.2. ASan/UBSan và imports PASS; AuxKC đã xác nhận 0.3.3,
  chưa chứng minh sửa lỗi ROM trên máy thật và chưa chạy mode presets.
- Sau reboot 0.3.3: `scripts/vm-test.sh --rom`, `scripts/vcpu-test.sh --modes`,
  `scripts/vz-probe.sh --start`. Native start vẫn có thể gặp API chưa triển khai tiếp theo.

## Checkpoint mới nhất: 0.3.4

- 0.3.3 runtime: VM/ROM pin fix PASS; native VZ start tới vcpu_create(flags=2).
- Protected32/long64 CPUID + RAM write PASS, resume fail do SVM bỏ saved CS.G.
  Diagnostic khôi phục G từ userspace làm HLT/page-walk NPF PASS, chưa tính driver PASS.
- 0.3.4 sửa khôi phục G và hỗ trợ HV_VCPU_TSC_RELATIVE + VMCB TSC offset/RDTSC.
  Build/imports/unit tests PASS, AuxKC đã xác nhận 0.3.4. Cần reboot để xác nhận runtime.
- Sau reboot: `scripts/vm-test.sh --rom`, `scripts/vcpu-test.sh --modes` (bao gồm tscprobe),
  `scripts/vz-probe.sh --start`. Chưa boot Linux hay đạt mục tiêu Podman/Docker.

## Checkpoint mới nhất: 0.3.5

- 0.3.4 đã PASS VM/ROM, real/protected32/long64 và TSC trên máy thật.
- Native VZ start qua vcpu_create(flags2), dừng ở managed MSR CSTAR (ID3).
- 0.3.5 hỗ trợ managed STAR/LSTAR/CSTAR/SFMASK/KERNEL_GS_BASE, cache userspace,
  guest RDMSR/WRMSR emulation; giữ MSR host qua VMSAVE/VMLOAD. Chưa runtime-test.
- Build/imports và tests PASS; AuxKC đã xác nhận 0.3.5. Sau reboot chạy VM --rom,
  vcpu --modes (bao gồm msrprobe), rồi vz-probe --start để xác định API còn thiếu.
- Chưa native EFI/Linux boot, chưa đạt Podman/Docker container/lifecycle/performance.

## Checkpoint mới nhất: 0.3.6

- 0.3.5 runtime: toàn bộ CPU/modes/TSC/syscall MSR gate PASS.
- Native VZ start còn thiếu set_msr_access và managed FS_BASE (ID148).
- 0.3.6 bổ sung quyền READ/WRITE cho managed MSR, alias FS_BASE/GS_BASE với VMCS/VMCB;
  test userspace và unit tests mở rộng. Build/imports/ASan/UBSan PASS; AuxKC xác nhận 0.3.6.
- Sau reboot 0.3.6: vcpu-test --modes (có MSR permissions/FS/GS), vz-probe --start;
  lưu trace API tiếp theo. Chưa có Linux boot hay Podman/Docker hoạt động.

## Checkpoint mới nhất: 0.3.7

- 0.3.6 đã PASS bộ thử CPU/modes/TSC/MSR permissions/FS/GS trên máy thật.
- Native VZ tới enable managed SYSENTER_CS (framework145), chưa tới vcpu_run.
- 0.3.7 thêm SYSENTER_CS/ESP/EIP qua VMCS/VMCB aliases, kiểm tra giá trị và test read/write.
  Build/imports/unit tests PASS; AuxKC xác nhận 0.3.7, chưa runtime-test.
- Sau reboot: `scripts/vcpu-test.sh --modes`, `scripts/vz-probe.sh --start` và lưu trace.
  SYSENTER MSR state không đồng nghĩa syscall transition hay Linux boot đã đạt.

## Quy trình giảm reboot (ưu tiên người dùng, 2026-11-07)

- Không cài bản kext mới và yêu cầu reboot chỉ vì sửa thêm một trap/MSR.
- Trước lần cài tiếp theo: khảo sát cả nhóm API startup của backend Apple từ SDK,
  disassembly và probes; gom các phần liên quan thành một đợt có kiểm thử đầy đủ.
- Mở rộng test userspace cho ABI, cache, validation, permission, exit translation và
  rollback. Các test này build/chạy lại không cần reboot; không thay thế chứng cứ SVM thật.
- Dùng kext đang nạp để chạy các probe độc lập và thu thập nhiều giới hạn trong cùng boot.
  Chỉ sửa userspace/probe thì không cài lại kext. Không trả thành công giả cho API chưa hỗ trợ
  để ép VZ đi tiếp, và không coi mock/diagnostic là native boot PASS.
- Sau khi đợt thay đổi đã sẵn sàng, mới cài AuxKC và yêu cầu một reboot để chạy toàn bộ
  hardware regression + native startup. Không cam kết số reboot còn lại trước kết quả.
- Hiện 0.3.6 đang nạp, 0.3.7 đã staged. Chưa yêu cầu reboot thêm trong lúc chuẩn bị đợt tiếp.
- Hot-unload không được triển khai: terminate() từ chối sau runtimePinned vì kernel đã
  nhận raw callback pointers. Muốn reload an toàn phải có cơ chế quiesce/in-flight lifetime
  được chứng minh; không bỏ guard hay hot-patch kernel để tiết kiệm một lần reboot.


## Checkpoint 0.3.8: gom native startup trước một lần reboot

- Thay staged0.3.7 chưa boot; loaded0.3.6. Bộ offline `scripts/check-offline.sh` PASS.
- Khảo sát native XPC và probe trên loaded kernel: toàn bộ nhóm 11 MSRs đã được
  nhận diện; 0.3.8 bổ sung ba SYSENTER + TSC, relative clock cache, run_until FOREVER.
- Có pure handler tests cho logic MSR thực; hardware gate mới chạy nativeinitprobe,
  modeprobe, tscprobe (thêm API/guest TSC và run_until), msrprobe (thêm SYSENTER).
- Entry diagnostics thu đầy đủ controls/CR masks/shadows/segments/FP state pending
  để lần boot sau xác định cả nhóm việc tiếp theo. FPU chưa hỗ trợ được reject rõ ràng.
- Native x86 call-site inventory cho thấy VMCS/capabilities, interrupt và exit handling
  là ưu tiên tiếp theo; chưa thấy kernel APIC calls trực tiếp trong binary này.
  Đây là suy luận từ static inventory, không loại trừ indirect calls hay nhu cầu APIC.
- Chỉ yêu cầu reboot sau khi AuxKC xác nhận0.3.8. Sau reboot: VM --rom, vCPU --modes,
  VZ --start và lưu logs. Mọi thay đổi0.3.8 còn cần kiểm chứng hardware.
- Mục tiêu giữ nguyên: Podman applehv/vfkit và Docker native backend thực chạy Linux
  containers; các preset/tests hiện tại chưa chứng minh Linux boot hay hoàn tất Phase3.

- Cài đặt hoàn tất: AuxKC xác nhận `com.fixvm.driver.AMDHV 0.3.8` tại
  `/Library/Extensions/AMDHV.kext`; `kmutil load` trả28 (requires reboot), script xác nhận
  staged thành công. Log `research/runs/0.3.8-install.log`.
  Hiện loaded0.3.6, takeover=0/vms=0/hv_support=0. Dừng tại bước reboot theo yêu cầu người dùng.
  Sau khi người dùng báo đã reboot, xác nhận loaded0.3.8 rồi chạy toàn bộ hardware gates nêu trên.


## Checkpoint 0.3.9: host cancellation và lifetime

- Loaded0.3.8 đã PASS toàn bộ VM/ROM/modes/MSR/TSC/run_until hardware gate.
  Native VZ qua11 MSRs, fail sau unsupported interrupt request; chưa có vcpu_run.
  Chưa xác định interrupt là nguyên nhân đầu tiên hay request trong cleanup.
- 0.3.9 hỗ trợ host cancellation bằng mailbox atomic độc lập global run gate,
  validation bitmap, coalescing, destroy/reuse cleanup; trả VMX exit1 không inject guest IRQ.
  Bounded single-step runner mới là cơ sở kiểm tra request; chưa có IPI/preemption backend
  cho guest unbounded. Đây là giới hạn phát triển, không phải hiệu năng production.
- Thêm unit concurrency test và hardware interruptprobe. ASan/UBSan + TSAN PASS;
  hardware0.3.9 chưa test. Snapshot khi destroy-before-run giúp chẩn đoán startup sớm.
- Sau khi AuxKC xác nhận0.3.9 và reboot: VM --rom, vCPU --modes (gồm interruptprobe),
  VZ --start. Nếu native fail, phân biệt startup failure với interrupt trong teardown
  trước khi sửa tiếp. FPU, CPU controls, general execution/interrupt injection vẫn còn thiếu.
- Phase3 và mục tiêu Podman/Docker native chưa hoàn tất.

- Cài0.3.9 hoàn tất: `kmutil load` trả28 yêu cầu reboot; AuxKC xác nhận
  `com.fixvm.driver.AMDHV 0.3.9 /Library/Extensions/AMDHV.kext`, script exit0.
  Log `research/runs/0.3.9-install.log`. Build không warnings; toàn bộ offline gate PASS
  trong `research/runs/0.3.9-offline-checks.log`.
  Máy vẫn loaded0.3.8, takeover0/vms0/hv_support0. Dừng chờ reboot theo yêu cầu.


## Checkpoint 0.3.10: native reset CRs và mask/shadow

- 0.3.9 CPU regression và host cancellation đã PASS hardware.
- Native đã tới run_until; entry bị reject do CR0=30/CR4=2000 và primary200080.
- 0.3.10 triển khai real-mode reset profile, bỏ VMXE khỏi AMD CR4, emulation CR0/4
  mask/shadow và VMX MOV-CR exit28 cho masked changes; thêm resetprobe tại high ROM vector.
- Giới hạn rõ: TPR-shadow chỉ trong real mode, không guest IRQ/APIC; unmasked mode changes,
  CR3/CR8, CLTS/LMSW chưa triển khai. Không đánh dấu EFI/Linux boot hoàn tất.
- Sau AuxKC0.3.10 và reboot: VM --rom, vCPU --modes (thêm resetprobe), VZ --start.
  Dùng exit diagnostics để xác định nhóm xử lý instruction/mode/interrupt tiếp theo.

- VM/ROM regression trên0.3.9 cũng PASS: `research/runs/0.3.9-vm-test.log`.
- Offline0.3.10 build/imports và tất cả unit tests PASS, không warnings:
  `research/runs/0.3.10-offline-checks.log`.
- Đã cài0.3.10: AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0, log `research/runs/0.3.10-install.log`. Máy vẫn loaded0.3.9,
  takeover0/vms0/hv_support0. Dừng chờ reboot theo yêu cầu người dùng.


## Checkpoint 0.3.11: descriptor tables và chuyển real→protected

- 0.3.10 VM/CPU/reset regression PASS. Native EFI thực thi đến LGDT với CS.base=ffff0000/RIP=ff4c
  (linearffffff4c), exit6b. Không tính là EFI/Linux boot hoàn tất.
- 0.3.11 gom SGDT/SIDT/LGDT/LIDT, save/restore/publish table state, validation;
  hỗ trợ PE0→1 và ring0 legacy cached16/32-bit segments để far jump vào protected mode.
- Thêm descriptorprobe kiểm thử toàn chuỗi và NPT fault operand. Offline gate PASS,
  hardware mới còn pending. Không có FPU, general paging transitions, IRQ/APIC đầy đủ.
- Sau AuxKC0.3.11 và reboot: VM --rom, vCPU --modes (thêm descriptorprobe), VZ --start;
  thu trace instruction/state tiếp theo. Mục tiêu Podman/Docker native vẫn chưa đạt.

- Cài0.3.11 hoàn tất: AuxKC xác nhận `com.fixvm.driver.AMDHV 0.3.11` tại
  `/Library/Extensions/AMDHV.kext`; kmutil load exit28 yêu cầu reboot, script exit0.
  Log `research/runs/0.3.11-install.log`. Loaded vẫn0.3.10; lần kiểm tra cleanup
  takeover0/vms0/hv_support0. Dừng chờ reboot theo yêu cầu người dùng.


## Checkpoint 0.3.12: legacy CR preparation và vòng lặp qua nhiều lượt chạy

- 0.3.11 VM/ROM/CPU/descriptors/PE transition PASS hardware. Native EFI đã qua LGDT,
  hiện reject Apple state CR0=23 sau masked MOV-CR exit, trước far jumpff5a.
- Audit firmware thật xác định cả nhóm bước tiếp theo; xem `research/efi-reset-25G332.txt`.
- 0.3.12 gom CR0 PE/MP/ET/NE, CR4 MCE/OSFXSR/OSXMMEXCPT/PAE trong legacy mode,
  CR3 staging trước paging và bounded host exit khi hết quantum1024steps.
- Thêm controlprobe gồm loop5000iterations/resume và kiểm tra FPU vẫn bị chặn.
  Offline PASS; hardware0.3.12 pending. Không hỗ trợ paging transition/EFER đầy đủ/FPU/IRQ/APIC.
- Sau AuxKC0.3.12/reboot: VM --rom, vCPU --modes (thêm controlprobe), VZ --start.
  Trace dự kiến có thể tới CPUID/EFER/paging, nhưng chưa khẳng định trước hardware.
- Goal Podman/Docker native và Phase3 chưa hoàn tất; single-step còn là cơ chế phát triển.

- Đã cài0.3.12: AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.12-install.log`. Loaded vẫn0.3.11,
  takeover0/vms0/hv_support0 trước cài. Dừng tại bước reboot theo yêu cầu người dùng.


## Checkpoint 0.3.13: LME preparation → compatibility → long code

- 0.3.12 CPU/legacy CRs/5000-iteration loop PASS hardware. Native EFI đã dựng bảng trang,
  VMM xử lý EFER read/write và dừng ở EFER100 trước bật paging.
- 0.3.13 hỗ trợ LME-preparation, IA-32e compatibility entry và far jump tới64-bit code,
  native CR0/CR4/selectors, VM-entry bit9; mở rộng validation và longtransitionprobe.
- Masked CR0.PG exit vẫn giao userspace VMM cập nhật PG/LMA/shadow/entry state.
  FPU/APIC/guest IRQ/general paging transitions chưa triển khai; EFI/Linux boot chưa đạt.
- Offline PASS, hardware mới pending. Sau AuxKC0.3.13/reboot: VM --rom,
  vCPU --modes (thêm longtransitionprobe), VZ --start; lưu toàn bộ exit/state tiếp theo.

- VM/ROM regression0.3.12 PASS: `research/runs/0.3.12-vm-test.log`.
- Cài0.3.13 hoàn tất, AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.13-install.log`. Loaded vẫn0.3.12,
  takeover0/vms0/hv_support0 trước cài. Dừng chờ reboot theo yêu cầu người dùng.


## Checkpoint 0.3.14: port I/O exits và guest instruction fetch

- 0.3.13 CPU/longtransition PASS hardware. EFI native thực sự chạy64-bit, dừng tại
  OUT byte DX→port3fb, RIPfffe150e. Chưa boot Linux.
- 0.3.14 gom scalar IN/OUT widths8/16/32, immediate/DX, string REP metadata và VMXexit30.
  Opcode được xác minh qua pinned memory + read-only guest page walker, không host I/O.
- Offline tests bao phủ decoder/memory permissions/page walks4K/2M/1G; ioprobe16/32/64
  đã build, hardware pending. Không coi unit tests là thiết bị/EFI boot thành công.
- Sau AuxKC0.3.14/reboot: VM --rom, vCPU --modes (thêm ioprobe), VZ --start;
  thu API/exit tiếp theo, tiếp tục gom thay đổi. FPU/IRQ/APIC/SMP/performance vẫn chưa xong.

- VM/ROM regression0.3.13 PASS: `research/runs/0.3.13-vm-test.log`.
- Cài0.3.14 hoàn tất, AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.14-install.log`. Loaded vẫn0.3.13,
  takeover0/vms0/hv_support0 trước cài. Dừng chờ reboot theo yêu cầu người dùng.

## Checkpoint 0.3.15: FPU context switching và observation gate

- 0.3.14 vCPU/I/O và VM/ROM hardware PASS. Native EFI đi qua port I/O rồi #NM ở
  WAIT/FNINIT tạifffe1353. Start callback Running chưa đủ; vzprobe nay quan sát15giây.
- 0.3.15 gom x87/MMX/SSE context switching, FP shared ABI, MXCSR validation và test
  host preservation. Offline PASS, gồm2000lượt thực thi FP-switch macros ở userspace.
- Sau kích hoạt0.3.15: chạy vCPU --modes (fpprobe mới), VM --rom, VZ --start;
  đọc cả state/delegate/trace và xác nhận cleanup. Không coi lifecycle là Linux boot.
- AVX/OSXSAVE guest, FPU exception injection, APIC/IRQ/SMP và performance vẫn chưa xong.
  Mục tiêu Podman/Docker native giữ nguyên; chưa chạy container.

- Cài0.3.15 hoàn tất: AuxKC xác nhận version/path đúng, kmutil load exit28 yêu cầu
  reboot, install script exit0. Log `research/runs/0.3.15-install.log`.
  Loaded vẫn0.3.14; takeover0/vms0/hv_support0. Dừng tại ranh giới reboot;
  sau reboot xác nhận loaded0.3.15 rồi vCPU --modes, VM --rom, VZ --start.

## Checkpoint 0.3.16: paging controls sau FPU

- 0.3.15 hardware vCPU/FPU/VM/ROM PASS. Native EFI vượt FPU và chạy mã trongRAM;
  dừng MOV CR0 tại1fff0d2c. Static DxeIpl evidence khớp việc bật WP.
- 0.3.16 thêm cập nhật CR0.WP/AM, CR4.PSE/PGE, thay CR3 khi long mode đã hoạt động,
  giữ masks/shadows và giới hạn mode. Offline PASS; chưa hardware-test bản này.
- Sau reboot: vCPU --modes (pagingcontrolprobe mới), VM --rom, VZ --start15giây;
  đọc rejected-CR operand nếu còn blocker, xác nhận cleanup và tiếp tục nhóm kế tiếp.
- IRQ/APIC/SMP, guest exceptions, AVX/extended state, general mode transitions và
  performance còn thiếu. Chưa đạt Linux boot hay Podman/Docker native.

- Cài0.3.16 hoàn tất: AuxKC đúng version/path, kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.16-install.log`. Loaded vẫn0.3.15;
  takeover0/vms0/hv_support0. Dừng ở bước reboot để kích hoạt bản mới.

## Checkpoint 0.3.17: EFER.NXE và quyền thực thi trang

- 0.3.16 vCPU/paging/FPU/VM/ROM hardware PASS. Native EFI vượt WP, dừng khi Apple
  publish EFER=d00 (NXE) vì mode validator cũ chưa cho phép bit này.
- 0.3.17 hỗ trợ NXE xuyên suốt mode/entry/CR/instruction fetch, giữ quyền NX tại
  mọi cấp và không cho phép các bit EFER khác chưa triển khai. Offline PASS.
- Sau reboot: vCPU --modes (nxprobe mới, pagingcontrolprobe500/d00), VM --rom,
  VZ --start15giây. Hardware0.3.17 pending; Linux/Podman/Docker chưa boot.

- Cài0.3.17 hoàn tất: AuxKC đúng version/path, kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.17-install.log`. Loaded0.3.16 chưa thay;
  takeover0/vms0/hv_support0. Dừng tại reboot, sau đó chạy các hardware gate nêu trên.

## Checkpoint 0.3.18: PUSHF/POPF và TLB trong bounded runner

- 0.3.17 hardware NXE/paging/FPU/VM/ROM PASS. VZ15s và120s lifecycle PASS; bài300s
  vượt giải nén DxeCore, tới PUSHF unsupported tại1fb401ad. Chưa Linux boot.
- Userspace vzprobe có `--start --seconds 1..300`, giúp tìm blocker xa hơn mà
  không đổi kext/reboot chỉ để tăng thời gian quan sát.
- 0.3.18 gom PUSHF/POPF bằng temporary execution breakpoint (ẩn internalTF), DF,
  INVLPG conservative flush và giảm flush TLB bên trong một quantum. Vẫn bounded1024;
  đầu mỗi run và sau CR writes/INVLPG phải flush. Offline PASS; hardware pending.
- Sau reboot: vCPU --modes (flagsprobe/tlbprobe/budgetprobe mới), VM --rom,
  VZ --start --seconds300; so baseline loop0.3.17 nhưng không kết luận performance
  từ một mẫu. Nếu flags test FAIL, đọc flags-step next/DR6/flags trước khi chạy VZ.
- Guest interrupts/exception injection, APIC/SMP, AVX, general privilege/mode
  transitions và production performance vẫn còn thiếu. Goal Podman/Docker chưa đạt.

- Cài0.3.18 hoàn tất: AuxKC đúng version/path; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.18-install.log`. Loaded vẫn0.3.17;
  takeover0/vms0/hv_support0. Dừng tại reboot để kích hoạt hardware gate mới.

## Checkpoint 0.3.19: PAUSE trong firmware spin loop

- 0.3.18 hardware vCPU/flags/TLB và VM/ROM PASS. Native EFI vượt PUSHF cũ,
  dừng PAUSE tại1fb401d8 trong vòng chờ port input; cleanup sạch, chưa Linux boot.
- 0.3.19 giữ intercept và consume PAUSE theo NRIP với budget1024, hỗ trợ boundary
  PUSHF/POPF→PAUSE. Offline PASS; probe thêm finite/infinite loops và state preservation.
- Sau reboot chạy vCPU --modes, VM --rom, VZ --start --seconds300. Hardware0.3.19
  chưa xác minh; goal Podman/Docker native vẫn chưa đạt.

- Cài0.3.19 hoàn tất: AuxKC đúngversion/path, kmutil load exit28 yêu cầu reboot,
  install script exit0. Log `research/runs/0.3.19-install.log`. Loaded vẫn0.3.18;
  takeover0/vms0/hv_support0. Dừng tại ranh giới reboot để kích hoạt bản mới.

## Checkpoint 0.3.19 hardware: PAUSE PASS, dừng tại guest IF=1

- 0.3.19 vCPU --modes (PAUSE/flags/TLB/budget) và VM --rom PASS trên máy thật.
- VZ300s: native EFI vượt PAUSE, chạy vào DxeCore; entry bị reject vì RFLAGS.IF=1
  (RIP1ffb1d9d, primary200080, injection0). STI chạy native vì SVM không intercept STI.
  Log `research/runs/0.3.19-vz-observe300.log`. Chưa Linux boot.
- Đợt tiếp theo (0.3.20) là guest interrupts: IF=1 + interrupt shadow, event/exception
  injection, interrupt/NMI-window exiting, HLT khi IF=1. Khảo sát ABI Apple VMM trước,
  gom thành một lần cài/reboot. IRQ/APIC/SMP và performance vẫn còn thiếu.

## Đối chiếu CachyOS/KVM (2026-11-10) → phạm vi 0.3.20

- Chứng cứ Zen5+KVM: `research/linux/KVM-INTERRUPT-REFERENCE.md` (13/13 PASS, không thay hardware gate macOS).
- 0.3.20 cần cùng lúc: IF trong PilotFlags; giữ VMCB INT_SHADOW qua run và ánh xạ 1 bit →
  0x4824 = IF ? STI(1) : MOVSS(2), không publish 3; EVENTINJ type0/2/3 + error code
  (reject type4/5/6 tới khi có next_rip); interrupt-window primary bit2 → VINTR exit7;
  EXITINTINFO → 0x4408/0x440a; reject injection khi có shadow/IF=0 như Apple VMM.
- Probe mới trên macOS: STI window, MOV SS→PUSHF/PAUSE boundary (MOV SS gộp 2 lệnh/bước),
  inject #GP/#UD/ext-int, HLT IF=1, shadow qua quantum boundary. Offline gate rồi mới cài/reboot.

## 0.3.20: IF/shadow và interrupt-window (2026-11-10)

- Đã đọc và đồng bộ chứng cứ CachyOS/KVM. Điều chỉnh phạm vi so với đề xuất Linux:
  chưa bật EVENTINJ. Runner dùng internal TF; delivery có thể lưu TF vào guest frame,
  đồng thời interrupt gate xóa TF có thể làm mất giới hạn thực thi handler. Cần cơ chế
  bounded event delivery, xử lý interrupted delivery/IRET/NMI trước khi hỗ trợ injection.
  Không coi phép ánh xạ field EVENTINJ là đủ. NMI blocking/window vẫn reject.
- Cho IF vào PilotFlags, validator chấp nhận interruptibility0/1/2; reject STI-shadow
  khi IF0, cả2shadowbits, NMI/SMI blocking. VMCB INT_SHADOW được nạp lại mỗi run;
  publish shadow thành IF?STI:MOVSS, không publish3. Đây là phép ánh xạ hạn chế của
  SVM1bit; không tuyên bố đầy đủ debug/NMI semantics.
- Primary bit2 cho interrupt-window: kiểm tra IF và shadow tại mỗi boundary trước
  VMRUN (trừ giữa flags one-shot), trảVMX7 trước thực thi lệnh tiếp theo nếu sẵn sàng.
  Không arm dummy V_IRQ, không inject event. Host IRQs vẫn off trong quantum và
  V_INTR_MASKING vẫn bật. IRQ/APIC delivery chưa được hỗ trợ.
- Các lệnh tự emulate CR/MSR/PAUSE/INVLPG phải clear shadow sau khi retire; exit chưa
  consume instruction giữ shadow. POPF cho IF, vẫn reject IOPL/guestTF.
- flagsprobe mới16/32/64: STI ởstep1024, shadow qua quantum, window sau đúng1lệnh,
  window đang mở không thực thi HLT, HLT IF1 giữVMXRIP, IF0 không mởwindow, PAUSE
  retire restored shadow, MOVSS→PUSHF không lộTF, POPF IF, reject injection/NMIstate.
- Budget1024 là số entry/step, không phải chính xác số lệnh: MOVSS có thể gộp2lệnh.
- Hardware0.3.20 pending. Sau reboot: vCPU --modes trước, VM --rom, VZ300s; nếu
  shadow/flags probe lỗi phải phân tích trước khi thửVZ. Linux/Podman chưa boot.

- Offline0.3.20 PASS, không warning: build/imports339, ASan/UBSan, FP preservation,
  resolver6cases/shellsyntax. Log `research/runs/0.3.20-offline-checks.log`.
- Cài0.3.20 PASS: AuxKC đúngversion/path; kmutil exit28 yêu cầu reboot, script exit0.
  Log `research/runs/0.3.20-install.log`. Loaded vẫn0.3.19, idle takeover0/vms0/hv_support0.
  Dừng tại reboot theo yêu cầu; chưa chạy hardware probe0.3.20.

## 0.3.21: guest debug-register save/restore (2026-11-10)

- Native0.3.20 vượt blockerIF. Static PE GUID1a1e4886-9517-440e-9fde-3be44cee2136
  RVAb20a khớp observedRIP1fa6320a với inferredbase1fa58000: MOVfromDR0/1/2/3/6/7,
  SGDT/SIDT rồi STR. Evidence `research/efi-debug-save-25G332.txt`, không runtime dump.
- 0.3.21 emulate MOVDR0..3/6/7 vào state riêng khi breakpoint tắt. Không chạy host
  MOVDR từ operand guest. Fetch/decode đúngNRIP sau khi restorehostIRQs; mỗi MOVDR
  consume một lệnh rồi trảhostexit1, không recurse. Giữwidth32 trong legacy và64
  trong code64, decodeREX.GPR; hỗ trợRSP riêng. Retire thì clear interruptshadow.
- DR0..3 virtual canonical address, DR6 không lộ internalBS/B0, normalize reserved
  fixedbits; DR7 normalizebit10 và chỉ chấp nhận disabled breakpoints, khôngGD.
  Guest breakpoint enable, DR4/5 alias, noncanonical address và unsupportedreserved
  writes reject trước commit. Không tuyên bố đầy đủ debug exception virtualization.
- PrimaryMOVDR-exiting bit23 trảVMX29 với qualification/length, RIP/operand chưa đổi.
  SLDT/STR được CPU thực thi trên guestVMCB selector (LDTR/TR vẫn fixed0);
  LLDT/LTR vẫn intercept/unsupported. ThêmDR exits vào flags one-shot boundary.
- debugprobe16/32/64: DR address roundtrip qua calls, resetDR6/7, REXr8/r9, reject
  breakpoint enable không đổiRIP/DR7, VMX29qual, shadowretirement/window, SLDT/STR.
  Unit decoder/state tests có truncation/mismatch/width/reserved/no-mutation cases.
- OfflinePASS khôngwarning: build/imports339, ASan/UBSan, FPU, resolver6cases và
  shellsyntax. Log `research/runs/0.3.21-offline-checks.log`. Hardware pending.
- Sau reboot: vCPU --modes (debugprobe mới), VM --rom, VZ300s. Injection IRQ/NMI,
  guest exception delivery, APIC/SMP và productionperformance vẫn thiếu; chưaLinuxboot.

- Cài0.3.21 PASS: AuxKC đúngversion/path; kmutil load exit28 yêu cầu reboot,
  installscript exit0. Log `research/runs/0.3.21-install.log`. Loaded vẫn0.3.20;
  takeover0/vms0/hv_support0. Dừng tại reboot theo yêu cầu người dùng.

## 0.3.22: bounded ring-0 IRQ entry và IRETQ (2026-11-10)

- Chứng cứ0.3.21 đã tới externalIRQ40. Không dùng hardwareEVENTINJ cùnginternalTF:
  interrupt gate có thể lưu TF vào frame rồi xóaTF khi vàohandler, phá boundedrun.
- Phạm vi0.3.22: event type0, vector32..255, code64/ring0, IF1/shadow0, gate8e/8f,
  IST0, selector cùngCS hiện tại và GDT descriptor64flat đãaccessed. Phạm vi khác
  (NMI/exceptions/softwareint/IST/privilegechange/highcanonical) vẫn rejectrõràng.
- Tạo frame bằng software trước khi thêmTF: alignRSP16 rồi pushSS/RSP/RFLAGS/CS/RIP
  (40bytes), interruptgate xóaIF/trapgate giữIF. Clearentryvalid sauconsume. Handler
  vẫn dùng boundedTF1024entries, không có đoạn hardware chạyTF0 khônggiới hạn.
- IRETQ48cf intercept được emulate riêng: đọc5qwords, cùngCS/SS, lowercanonical
  RIP/RSP, flags thuộcPilotFlags; commit sau validation. Trảhostexit1 sauIRETQ,
  run tiếp ởđịa chỉreturn. Không hỗ trợNMI-unblock/taskreturn/privilegechange.
- EventMemoryPlan walker4level/2MiB/1GiB: canonical/PTEreserved/NXE/WP/RWparents;
  dataNX được đọc khiNXE1. Lập trước danh sách A/D updates và40framebytes, kiểm tra
  tất cả NPT/hostpin writepermissions trướcstore. Không dereference guestphysical
  thànhhostpointer. GuestMemory.writePhysical chỉghi prepared writable live mappings,
  preflight range trướcwrite; mock/unit cover readonlysecondpage không partialwrite.
- Deliveryfault/thiếumapping/không hỗ trợ trảHV_UNSUPPORTED, chưa tổng hợp #PF/#GP,
  VMXvectoring/NPFretry cho delivery. Không tuyên bố full interrupt virtualization.
- eventprobe mới: guestGDT/IDT/handler thật, IRQ→handler→IRETQ→HLT, savedframe và
  handlerPUSHF khôngTF, interrupt/trapIF, restoreunalignedRSP/flags, eventconsumeonce,
  NPTreadonlysecondstackpage khôngghi framefragment, rejectIST/NMI/IRETbadflags.
- Unit/ASan/UBSan coverframealignment/A-D,4K+hugepaging,parentRW,WP0/1,NX data/reserved,
  unreadableIDT,nonpresentCS,readonlyPTE metadata, preflight khôngstate/frame mutation.
- OfflinePASS khôngwarning, imports339, FP-preservation/resolver6cases/shellsyntax:
  `research/runs/0.3.22-offline-checks.log`. Hardware0.3.22 pending reboot.
- Sau reboot: vCPU --modes (eventprobe cuối), VM --rom, VZ --start --seconds300.
  NếueventprobeFAIL, phân tíchframe/IRET trướcVZ. Mục tiêuPodman/Docker chưa đạt.

- Cài0.3.22 PASS: AuxKC đúngversion/path; kmutil load exit28 yêu cầu reboot,
  installscript exit0. Log `research/runs/0.3.22-install.log`. Loaded vẫn0.3.21;
  takeover0/vms0/hv_support0. Dừng tại reboot theo yêu cầu người dùng.

## Đánh giá Graft/NVMM theo repo người dùng gửi (2026-11-10)

- Đã đọc milouk/graft commit15e1f2a… và milouk/AMD-Ryzen-Hackintosh.
  Graft dùng NVMM.kext + VMM riêng, Linux directboot/container CLI; chưa cung cấp
  AppleHV/VZ ABI. Repo EFI cũng ghi Docker Desktop/AppleHV apps chưa hoạt động.
- Build/unit riêng PASS trên SDK15.x và26.5; NPT9259checks/9P786800checks PASS,
  importKDK25G332 PASS55symbols. ChưaloadNVMM/chưa xác minh Linux/container runtime.
- Kết luận và mapping áp dụng: `research/external/GRAFT-ASSESSMENT.md`.
  Ưu tiên khảo sát mô hình noTF/preemption/hostIRQ và hardwareeventdelivery, đồng thời
  giữ mục tiêu Apple ABI. Tích hợp NVMM cần adapter đáng kể, không drop-in thay kext.
- Không thay EFI, không load runner thứ2, không thay checkpoint0.3.22 đãstage chờreboot.

## 0.3.23: CR8/TPR và chế độ no-TF opt-in (2026-11-10)

- TừblockerCR8 của0.3.22: MOVCR8 được emulate bằng HV_X86_TPR(id49), CR8=TPR>>4,
  writeCR8 chỉ0..15→TPR=value<<4; không chạmhostCR8. BitCR8load/store-exiting
  primary20/19 trảVMX28 trướcconsume. TPRthresholdnonzero vẫnunsupported.
- Áp dụng mô hình hostIRQ/noTF đã đối chiếuGraft/KVM bằng thay đổi runner riêng,
  không vendoring NVMM. `debug.amdhv.fast=0` mặcđịnh; root chỉđổi khi takeover0/vms0.
  Fastmode bỏinternalTF, choPUSHF/POPF/IRET chạyCPU và externalEVENTINJ phầncứng.
- Assembly nhậnarg7hostIRQ (offset120). IF1 chỉsauCLGI/GIF0, STI cách xaVMRUN để
  tránhhostSTIshadowbleed. CLI ngay sauVMRUN/GIF0, hoststateFP/CR0/segments restore
  trướcSTGI/IF0; C++ restoreDR/EFER/HSAVE trước bậtIRQhost. KhôngmigrateCPU trongquantum.
- HostINTR/NMIexit trảVMX1; cap32handledexits. KhôngharddeadlinegiữaVMEXITs; cầntimer
  hardwaregate trướcguestvôhạn. Cancellation chờhostIRQ/mỗientry, chưa cóIPI kick.
- FastVINTRwindow phầncứng vớiV_IRQ+IGN_TPR vàinterceptVINTR; IRQentryđược ưu tiên
  trướcwindowcheck. EVENTINJclear sauVMRUN đểkhônginjectlặp khihandledexit. EXITINTINFO
  publishVMX4408/440a đểVMM xửlýinterrupted delivery; chưahardwareverifyAppleABI này.
- fastprobe: finite200triệu loop phải chứngminhtimerexits trước10lầninfiniteJMP(IF0/1),
  call<500ms làtestcriterion, khônglàguarantee; mailboxcancel, CR8/TPR, CR8loadexit,
  VINTR sauSTI, hardwareIRQ→handler→IRETQ cùngframe/PUSHF khôngTF. Hardwarepending.
- Script `scripts/fast-test.sh` kiểmtra mode khôngđổi khiactive vàrestorefast/takeover;
  VZ có `--fast`, tựrestoremode khi cleanup. Default vCPUgate yêu cầufast0 và cóCR8tests.
- OfflinePASS khôngwarning, imports339, ASan/UBSan/FP/resolver6cases/scriptsyntax.
  Log `research/runs/0.3.23-offline-checks.log`. Compiledcallerarg7store(%rsp),
  callee0x78(%rsp), CLI đầu tiên sauVMRUN đãđốichiếu, offsetsFP/GPR/PAs cũ giữnguyên.
- Phân tích/giớihạn/hardwaregate: `research/external/NO-TF-RUNNER.md`.
  Sau reboot: loaded0.3.23→vCPU --modes→VM --rom→fast-test. ChỉkhifastPASS mới
  `vz-probe.sh --fast --start --seconds300`. CPL0/lowercanonical/noAVX và nhiều
  giớihạn khác còngiữ; Linux/Podman/Docker chưahoànthành.

- Cài0.3.23 PASS: AuxKC đúngversion/path; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.23-install.log`. Loaded vẫn0.3.22;
  takeover0/vms0/hv_support0. Dừng tại reboot. Fastmode chưa kiểmthử hardware,
  không bật mặcđịnh; sau reboot theo hardwaregate trongNO-TF-RUNNER.md.

## 0.3.23 hardware verification và 0.3.24 CR4.DE (2026-11-10)

- Sau reboot xác nhận loaded0.3.23, takeover0/vms0/fast0/hv_support0.
- vCPU --modes PASS toàn bộ (gồm CR8/TPR và softwareIRQ/IRET); VM/ROM PASS.
  Logs: `research/runs/0.3.23-vcpu-test.log`, `0.3.23-vm-test.log`.
- Fast hardware gate PASS: finite200triệu loop có22 physicalhostIRQ exits,
  worstcall5.739ms trong lượt đo này; infiniteJMP IF0/IF1, mailboxcancel,
  CR8roundtrip/VMX28, VINTR/STIwindow, hardwareIRQ/IRET interrupt+trapgate PASS.
  Log `research/runs/0.3.23-fast-test.log`. Đây là phép đo, không guarantee deadline.
- VZfast với observation tối đa300s FAIL trước khi kết thúc observation:
  firmware vượtCR8read cũ rồi ghiCR4=668 (thêmDEbit3 vào660) tạiRIP1fa59425,
  SVMexit14/NRIP1fa59428. Validator rejectDE; không phải timer/IRQ gatefail.
  Log `research/runs/0.3.23-vz-fast300.log`. Cleanup0/0/0/0; chưaLinuxboot.
- 0.3.24 cho phépCR4.DE trong sharedmode/CRwritepolicy. MọiMOVDR vẫnintercept,
  guestDR7 enables/GD vẫnreject; DR4/DR5 vẫnunsupported thay vì giả lập#UD.
  Không bật hostbreakpoints, không mở rộng exceptioninjection/NMI/privilege support.
- Unit coverDEenable/read/re-entry/disable và maskedwrite khôngcommit. Hardware
  probes mới: step16/32/64 bậtDE trước cácDRtests; fastDEroundtrip rồi IRQ/IRET/TPR
  vớiDEgiữbật. Cần xác minh trênloaded0.3.24 sau reboot.
- Offline0.3.24 PASS, khôngwarning: build/imports339, ASan/UBSan, FPpreservation,
  resolver6cases, scriptsyntax. Log `research/runs/0.3.24-offline-checks.log`.
- Sau reboot0.3.24: vCPU --modes → VM --rom → fast-test → chỉkhiPASS mới
  vz-probe.sh --fast --start --seconds300. Mục tiêuPodman/Docker chưahoànthành.

- Cài0.3.24 PASS: AuxKC đúngversion/path, kmutil exit28 yêu cầureboot;
  installscript exit0. Log `research/runs/0.3.24-install.log`.
  Loaded vẫn0.3.23; takeover0/vms0/fast0/hv_support0. Dừng tại reboot theo yêu cầu.

## Kiểm chứng 0.3.24 và bản sửa CR2 trong 0.3.25 (2026-11-10)

- Loaded 0.3.24 sau reboot; takeover/vms/fast/hv_support đều 0 trước kiểm thử.
- Toàn bộ vCPU --modes và VM --rom PASS. CR4.DE bật/đọc lại thành công ở
  guest 16/32/64 bit, sau đó các bài thử DR và IRQ/IRET đều PASS.
  Logs: `research/runs/0.3.24-vcpu-test.log`, `0.3.24-vm-test.log`.
- Fast gate PASS, gồm CR4.DE và hardware IRQ/IRET. Bài finite loop có 17 host IRQ
  exits, worst call 8.547 ms trong lượt đo này, không phải cam kết deadline.
  Log: `research/runs/0.3.24-fast-test.log`.
- VZ fast FAIL trước khi hết observation tối đa 300 giây: đã vượt CR4.DE,
  dừng ở MOV RAX,CR2, SVM exit=2 tại RIP=1fa5942d, NRIP=1fa59430, CR4=668.
  Log: `research/runs/0.3.24-vz-fast300.log`. Cleanup sạch, chưa boot Linux.
- Đã trích xuất và đối chiếu module firmware GUID 1a1e4886-9517-440e-9fde-3be44cee2136:
  prologue IRQ đọc CR2 ở RVA142d; epilogue ghi lại CR2 ở RVA14b6. Các thao tác CR/DR
  lân cận đã có handler. Đây là suy luận từ firmware tĩnh, không phải runtime dump.
  Báo cáo: `research/efi-cr2-save-25G332.txt`.
- 0.3.25 bổ sung CR2 ở framework register38: đọc/ghi bằng trạng thái riêng của vCPU,
  legacy write lấy 32 bit, IA-32e giữ 64 bit. Khởi tạo VMCB.CR2 mỗi run, cập nhật sau
  MOV CR2 và lấy snapshot khi VMEXIT. Host CR2 vẫn được lưu/khôi phục trước bật IRQ.
  Bỏ điều kiện CR2 phải bằng 0 trong validator; không dereference CR2 thành host pointer.
  Chưa bổ sung exception/#PF delivery hoặc mở rộng phạm vi CPL/địa chỉ RIP/RSP.
- Unit cover decode CR2, legacy width, full 64-bit payload, mode validation,
  framework pull/publish và trạng thái vCPU độc lập. Hardware probes mới cover
  guest MOV CR2/readback qua nhiều run và framework write; fast probe cũng cover
  full 64-bit payload. Các bài hardware mới cần loaded 0.3.25 sau reboot.
- Offline 0.3.25 PASS, không warning: build/imports339, ASan/UBSan, FP preservation,
  resolver 6 cases, script syntax. Log: `research/runs/0.3.25-offline-checks.log`.
- Sau reboot: vCPU --modes → VM --rom → fast-test → nếu tất cả PASS mới chạy
  `scripts/vz-probe.sh --fast --start --seconds 300`. Chưa đạt goal Podman/Docker.

- Cài 0.3.25 PASS: AuxKC đúng version/path, install script exit0; kmutil exit28
  yêu cầu reboot. Log: `research/runs/0.3.25-install.log`. Loaded vẫn 0.3.24;
  takeover/vms/fast/hv_support đều 0. Dừng tại bước cần reboot theo yêu cầu người dùng.

## 0.3.25 hardware và rà rộng trước 0.3.26 (2026-11-10)

- Loaded0.3.25; vCPU --modes / VM --rom / fast-test đều PASS, gồm CR2 read/write,
  CR4.DE, timer/IRQ/IRET. Log `research/runs/0.3.25-{vcpu-test,vm-test,fast-test}.log`.
- EFI tự tắt: log gốc `0.3.25-vz-fast300.log`; vzprobe được bổ sung guest-stop callback,
  console và elapsed time. Lượt có console xác nhận guestStop=1 sau0.227s, console0bytes.
  `0.3.25-vz-console60.log`. Static ResetSystemRuntimeDxe khớp bốn IO RIP cuối,
  gọi đường shutdown qua port604; chưa xác định vì sao firmware chọn shutdown.
- Đã quét70 module PE và rà kỹ CpuDxe, IRQ context, Linux bootstrap32/64.
  Báo cáo đầy đủ, phạm vi/giới hạn inventory và matrix tính năng:
  `research/FIRMWARE-AUDIT-0.3.26.md`. Không coi scan tĩnh là runtime/reachability proof.
- Linux direct boot bằng VZLinuxBootLoader, assets Alpine riêng, không disk/network,
  bị chặn ở LLDT10000f5. Rà tiếp thấy LTR10000fd và null DS/ES/SS/FS/GS đầu64bit.
  Log `research/runs/0.3.25-vz-linux60.log`; chưa boot Linux thành công.
- 0.3.26 gom cả LLDT/LTR, cache LDTR/TR qua run, null LDTR, null data segments64bit;
  mở rộng longtransitionprobe để giữ TSS qua32→64. Native GDT access/busy write chịu
  NPT; host VMLOAD restore trong GIF0. Task switch/CPL3/exception/NMI đầy đủ vẫn chưa hỗ trợ.
- Gate systemprobe mới cho step+fast: LLDT null/non-null, LTR/STR/SLDT, busy TSS reject,
  readonly busy-bit store NPF, unmapped GDT NPF, null data selectors64bit và re-entry.
  Unit cover mode/cache validation và invalid/canonical/limit/type cases.
- Offline0.3.26 PASS khôngwarning, imports339, ASan/UBSan, FP preservation,
  resolver6cases/scriptsyntax. `research/runs/0.3.26-offline-checks.log`.
  Các gate hardware mới pending loaded0.3.26. Chưa đạt goal Podman/Docker.
- Sau reboot: vCPU --modes → VM --rom → fast-test; chỉ nếu PASS mới thử VZ Linux
  direct boot tiếp. Cần tiếp tục rà rộng lỗi mới trước khi chốt các bản sau.

- Cài0.3.26 PASS: AuxKC đúngversion/path; kmutil exit28 yêu cầu reboot,
  installscript exit0. `research/runs/0.3.26-install.log`. Loaded vẫn0.3.25;
  takeover/vms/fast/hv_support đều0. Dừng tại reboot sau khi hoàn tất lượt rà rộng.

## 0.3.26 hardware, chẩn đoán rộng segment và 0.3.27 (2026-11-10)

- Loaded0.3.26; gate vCPU thường FAIL tại systemprobe. Native LTR ghi GDT busy đúng,
  nhưng saved TR attribute89 không có busy bit, khiến run tiếp bị validator từ chối.
  Log `research/runs/0.3.26-vcpu-test.log`. Không chạy VZ khi gate fail.
- Dùng probe chẩn đoán qua framework API để tiếp tục kiểm tra trên bản đang nạp:
  chỉ sửa TR làm lộ thêm null SS attribute4000 (DB1/not-present) chưa map unusable.
  `research/runs/0.3.26-system-diagnostic.log`. Đây không phải sửa/hotpatch kernel.
- Rà toàn nhóm8 cache fields với Linux/KVM svm_get_segment, kiểm tra firmware hash
  không đổi so với inventory70 PE trước đó. Báo cáo scope/bằng chứng/giới hạn:
  `research/SEGMENT-AUDIT-0.3.27.md`.
- 0.3.27 gom TR busy, not-present→unusable/null SS DB, present data accessed,
  dùng helper chung với G reconstruction cũ. Không nới input validators, không thêm
  CPL3/exception injection. Host VMLOAD/IRQ restoration không đổi.
- Sửa fixture systemprobe64: guest PTE nonpresent là #PF, không phải NPF; tách thêm
  guest-present/NPT-absent để kiểm tra NPF đúng. Không đổi kernel để che lỗi fixture.
- Chẩn đoán đầy đủ qua userspace PASS cả systemprobe32/64 và toàn vCPU suite,
  gồm longtransition với TR; fast suite chẩn đoán PASS (35 hostIRQ exits,
  worstcall5.297ms). Logs `0.3.26-segment-diagnostic.log`, `0.3.26-vcpu-diagnostic.log`,
  `0.3.26-fast-diagnostic.log`. **Không coi đó là PASS gate thường của kext0.3.26/27.**
- VM/ROM thường PASS: `research/runs/0.3.26-vm-test.log`.
- Offline0.3.27 PASS khôngwarning/imports339/ASan/UBSan/FP/resolver6/script syntax:
  `research/runs/0.3.27-offline-checks.log`. Unit tái hiện đúng hai saved-state images.
- Sau reboot0.3.27 cần gate thường không diagnostic: vCPU --modes → VM --rom →
  fast-test; chỉ khi PASS mới VZ Linux directboot. Chưa Linux/container thành công.

- Cài0.3.27 PASS: AuxKC đúngversion/path; kmutil exit28 yêu cầu reboot,
  installscript exit0. `research/runs/0.3.27-install.log`. Loaded vẫn0.3.26;
  takeover/vms/fast/hv_support đều0. Dừng tại reboot sau rà rộng và chẩn đoán phần cứng.

## 0.3.27 hardware PASS và nhóm high kernel entry/SCE cho0.3.28 (2026-11-10)

- Loaded0.3.27; toàn bộ gate thường vCPU --modes / VM --rom / fast-test PASS,
  không userspace normalization. Logs `research/runs/0.3.27-{vcpu-test,vm-test,fast-test}.log`.
- VZ Linux direct boot thực thi kernel CPUID tạiffffffff810441b1, rồi bị validator
  lower-half chặn ởffffffff810441b3. Chưa boot hoàn tất/console0bytes.
  Log `research/runs/0.3.27-vz-linux60.log`; teardown sạch.
- Rà runtime→ELF của đúng bzImage, kernel head/earlyIDT/initial_code và các block kế:
  tìm trước EFER.SCE enable ngay sau điểm dừng. Artifact `research/linux/alpine-high-entry-audit.txt`.
  Scope/chi tiết và matrix nhóm: `research/HIGH-ENTRY-AUDIT-0.3.28.md`.
- 0.3.28 gom canonical48 RIP/RSP, PAUSE/flags target, IRQ/IRET software high addresses,
  high fetch tests và fast-only EFER.SCE. Step vẫn rejectSCE vì SYSCALL/SFMASK có thể
  xóa internalTF. Không thêm CPL3/LA57/full exception/APIC/SMP.
- Gate eventprobe --high mới cho step; fast thêm --hardware và native guestSYSCALL
  STAR/LSTAR/SFMASK. Unit cover canonical hole/wrap/NX/NPT/atomic frame và SCE guards.
  Hardware các gate mới pending loaded0.3.28.
- Offline0.3.28 PASS, khôngwarning/imports339/ASan/UBSan/FP/resolver6/script syntax.
  `research/runs/0.3.28-offline-checks.log`. Sau reboot chạy gate thường đầy đủ,
  chỉ khi PASS mới tiếp tục VZ Linux. Chưa đạt Podman/Docker/container.

- Cài0.3.28 PASS: AuxKC đúngversion/path, kmutil exit28 yêu cầu reboot,
  installscript exit0. `research/runs/0.3.28-install.log`. Loaded vẫn0.3.27;
  takeover/vms/fast/hv_support đều0. Dừng tại reboot sau rà nhóm địa chỉ và SCE.

## 0.3.28 hardware PASS và nhóm guest faults cho0.3.29 (2026-11-10)

- Loaded0.3.28, toàn bộ vCPU --modes / VM --rom / fast-test PASS, gồm high-address
  và SCE/SYSCALL. Logs `research/runs/0.3.28-{vcpu-test,vm-test,fast-test}.log`.
- VZ Linux vượt SCE, vào boot-data copy rồi bị chặn ở guest#PF tại
  ffffffff82e3c985, faultVAffff888000001000, error0. Chưa console/boot hoàn tất.
  `research/runs/0.3.28-vz-linux60.log`; teardown sạch.
- Rà guest#PF vs NPT, Apple exception bitmap sites, Intel direct-exit CR2 và
  exception reinjection/error/vectoring. Báo cáo `research/EXCEPTION-AUDIT-0.3.29.md`.
- 0.3.29 cho native synchronous faults trong fast code64 theo bitmap, VMX0 cho
  intercepted faults, type3 reinjection kèm errorcode, constant PF mask/match,
  RF fast, vectoring/NPT và triple-fault VMX2. Nonconstant PF mask vẫn reject.
  Không thêm debug/NMI/machine-check/CPL3/LA57 hay exception delivery trong step.
- Đồng bộ MOV CR2 từ VMCB trong cùng quantum sau native#PF; direct VMX#PF exit
  giữ CR2 trước entry theo Intel, faultVA nằm ởqualification. Không đổi lỗi#PF thànhNPF.
- faultprobe mới cover nativePF/UD/GP, PTE+CR3/IRET/retry, bitmapVMX0/CR2/error,
  reinjectionerror0/nonzero, constantfilterinversion, deliveryNPF và triplefault.
  systemprobe cập nhật negative fastcode64 sang bitmapVMX0, giữ step/fast32 cũ.
- Offline0.3.29 PASS khôngwarning/imports339/ASan/UBSan/FP/resolver6/scriptsyntax:
  `research/runs/0.3.29-offline-checks.log`. Hardware gate mới pending loaded0.3.29.
- Sau reboot: vCPU --modes → VM --rom → fast-test; chỉ khi PASS mới tiếp tục
  VZ Linux directboot. Chưa đạt Podman/Docker/container.

- Cài0.3.29 PASS: AuxKC đúngversion/path; kmutil exit28 yêu cầu reboot,
  installscript exit0. `research/runs/0.3.29-install.log`. Loaded vẫn0.3.28;
  takeover/vms/fast/hv_support đều0. Dừng tại reboot sau rà nhóm exception ABI.

## 0.3.29 hardware PASS và nhóm CPU-init cho0.3.30 (2026-11-10)

- Loaded0.3.29; toàn bộ gate thường vCPU --modes / VM --rom / fast-test PASS,
  gồm nativePF/UD/GP, bitmap/error/reinjection, CR2, vectoringNPF và triplefault.
  Logs `research/runs/0.3.29-{vcpu-test,vm-test,fast-test}.log`.
- Default VZ Linux vượt lỗiREP MOVSD cũ, vào CPU setup rồi bị chặn ở
  MOVCR4 ffffffff81060f16, operand100b0 (FSGSBASE). Fail0.132s, console0bytes,
  teardown sạch. `research/runs/0.3.29-vz-linux60.log`. Chưa Linux/container proof.
- Trước chốt bản cài đã dùng command-line chẩn đoán để tìm tiếp SMEP, OSXSAVE
  và INT3selftest. Bốn log `0.3.29-vz-diag-*.log` đều là diagnosticFAIL,
  không thay thế gate thường. `nofsgsbase nosmep nosmap clearcpuid=umip` vẫn gặpSMEP;
  `clearcpuid=fsgsbase,smep,smap,umip` mới vượt nhóm này để gặpOSXSAVE;
  thêmnoxsave mới tớiINT3. Không sửa Linuxbinary/Podmandisk/loadedkext.
- Báo cáo đầy đủ `research/CPU-INIT-AUDIT-0.3.30.md`, exactELFdisassembly
  `research/linux/alpine-cpu-init-audit-0.3.30.txt`. Rà thêm70PE firmware,
  hash khôngđổi; scanner thêmFSGSBASE/STAC/CLAC, khôngdecodedsite trongnhóm này.
  Linear inventory không phải reachability proof; TE/raw vẫnngoài phạmvi.
- 0.3.30 gom nativeFSGSBASE/SMEP/SMAP/UMIP fastcode64, checkphysicalCPUID7,
  giữVMXmask/shadow; ACfast choSTAC/CLAC; softwarefetch chịuSMEP U/S mọi cấp;
  nativeINT3 khi bitmap3clear. Bitmap3set/type6reinjection vẫnUnsupported.
- faultprobe mở rộng baseinstruction/FS-GS memory/SWAPGS/managedMSR re-entry,
  disabledUD/noncanonicalGP, UMIP+CPL0SGDT, SMEPfetchPF, SMAPreadPF,
  STAC-ACexit/re-entry-CLAC, CR4maskexit và OSXSAVEreject. Hardware mới pending0.3.30.
- **OSXSAVE là blocker đã biết của Linux mặc định, chưa sửa trong0.3.30.**
  Cần extendedFPU/XCR0 switch riêng; không nới bit vì guest512byte FXSAVE chưa
  giữ AVX/extendedstate. Không tuyên bố nativeCPUfeatures là goal hoàn tất.
- Offline0.3.30 PASS khôngwarning/imports339/ASan/UBSan/FP/resolver6/scriptsyntax:
  `research/runs/0.3.30-offline-checks.log`.
- Sau reboot: gate thường vCPU --modes → VM --rom → fast-test. Chỉ khiPASS mới
  defaultVZLinux; nếu cònOSXSAVE, dùngdiagnosticnoxsave có nhãn để rà các phần
  sau trong lúc pháttriểnextendedstate. Không dùng nó làm cấu hình cuốiPodman/Docker.

- Cài0.3.30 PASS: AuxKC đúngversion/path; kmutil exit28 yêu cầu reboot,
  installscript exit0. `research/runs/0.3.30-install.log`. Loaded vẫn0.3.29;
  takeover/vms/fast/hv_support đều0. Backup kext0.3.29:
  `research/kext-backups/AMDHV-0.3.29.kext`. Dừng tại bước cần reboot.

## 0.3.30 hardware PASS, Linux console và XSAVE0.3.31 (2026-11-11)

- Loaded0.3.30 đúngversion; vCPU --modes / VM --rom PASS. Fast lượtđầu chỉFAIL
  vì fixtureSMEP mongPFEC15 thayvì11 (CPL0fetch/P+I-D, khôngU-S). Đã đối chiếu
  Intel/Linux, chỉ sửa kỳvọnguserspace; không sửa kext. Loggốc giữở
  `research/runs/0.3.30-fast-test-initial-fixture-fail.log`. Gatefast chạy lạiPASS.
  Logs thường `research/runs/0.3.30-{vcpu-test,vm-test,fast-test}.log`.
- DefaultLinux vượt nhómCR4 cũ, dừngOSXSAVE tạiCR4=350eb0; bitmap0.
  `research/runs/0.3.30-vz-linux60.log`.
- Diagnosticnoxsave đã có1130bytesconsole thật, driver/network init, freeinitrd,
  rồi bị validator chặnCPL3 (CS33/a0fb,SS2b/c0f3,RIP7ff66f87b125). Fail0.427s,
  teardown sạch. `research/runs/0.3.30-vz-diag-noxsave.log`.
  **ChưaLinuxboot hoàn tất/container. CPL3 là điểm chặn kế đã biết.**
- Đã rà cả XSAVE/XRSTOR, XSETBV/XGETBV, legacyFPABI, re-entry/reset, XSAVES/XRSTORS,
  hostXSS/XFD vàprivilegedtransition. Chi tiết `research/XSTATE-AUDIT-0.3.31.md`.
  HostXCR0=7, CPUID.D.2 offset576/size256; `0.3.30-xsave-layout.log`.
- 0.3.31 thêm boundedXSAVE x87/SSE/AVX chofastcode64, guestXCR0=1/3/7;
  hostXCR0 phải7, hostXSS/XFD phải0 nếuphysicalfeaturetồn tại, checkAVXlayout.
  Không mởAVX512/PKRU/CET/supervisorstate hoặcCPL3. InvalidXSETBV vẫnUnsupported,
  chưa tổnghợpGP đầyđủ. Step/legacy giữlegacyFPpath.
- SharedFPwindow vẫn512bytes; tail320bytes privateper-vCPU lưuheader/YMM;
  legacyAPIrewrite giữtail. AssemblyGIF0 savehost7/restoreguest7, setguestXCR0;
  mọiVMEXIT restorehostXCR07 trướcguestXSAVE/hostXRSTOR vàtrướcSTGI. Frameoffset
  khôngđổi. Đãràcompiledassembly, pointerRCX được giữ trướcxóaECXchoXSETBV.
- Offline fourFPvariants legacy/XSAVE, normal/init:1000mutations mỗi bàiPASS;
  unitcoverXCR0/CR4/tailbounds/reset/isolation. Newxsaveprobe coverAVX/re-entry,
  legacyAPIrewrite, gueststandard/compactedXSAVE, XCR0disable/re-enable,
  invalidinputs, reused-vCPU vàhostFP/XCR0restore. Hardwaremới pending0.3.31.
- Offline0.3.31 PASS khôngwarning/imports339/ASan/UBSan/fourFPvariants/resolver6/
  scriptsyntax. `research/runs/0.3.31-offline-checks.log`.
- Sau reboot: gate thường vCPU --modes → VM --rom → fast-test (xsaveprobe cuối).
  Chỉ nếu tấtcảPASS mớidefaultLinux khôngnoxsave. Ghi rõ điểmchặnthực tế,
  CPL3 hoặcguestxstate ngoài7 đã được nhận diện trước, không coiRunninglàbootproof.

- Cài0.3.31 PASS: AuxKC đúngversion/path, installscript exit0, kmutil exit28
  yêu cầu reboot. `research/runs/0.3.31-install.log`. Loaded vẫn0.3.30;
  takeover/vms/fast/hv_support đều0. Backup0.3.30:
  `research/kext-backups/AMDHV-0.3.30.kext`. Dừng tại reboot theo yêu cầu.

## 0.3.31 hardware PASS và extended xstate0.3.32 (2026-11-11)

- Loaded0.3.31; gate thường vCPU --modes / VM --rom / fast-test đềuPASS,
  gồmprivilegedXCR0=1/3/7, AVX, standard/compactedXSAVE, resetisolation,
  invalidinput vàhostFP/XCR0restore. Logs `research/runs/0.3.31-{vcpu-test,vm-test,fast-test}.log`.
- DefaultLinux dừngXSETBV ởffffffff8105a2a7/NRIP8105a2aa, CR4=350eb0,
  bitmap0, fail0.170s, console0bytes. `0.3.31-vz-linux60.log`. Bản31 chưa log
  EDX:EAX/ECX nên không coi numericXCR0request là số đo đã có.
- Diagnosticclearcpuid=pku vẫnfailcùngđiểm; clearcpuid=avx512f,pku vẫn giữXSAVE,
  tớiXCR0=7 và1130bytesconsole, rồiCPL3 CS33/SS2b/RIP7fe221c08125 bịvalidatorchặn,
  fail0.449s. Logs `0.3.31-vz-diag-{nopku,xstate7}.log`. ChưaLinux/containerbootproof.
- Rà đầyđủ component2/5/6/7/9, protectionkeysPKE/PKRU, disabledhoststate,
  XCR0expansion/restoration, layout/metadata bounds vàlegacyFPABI. CPUIDphysicalmask2e7,
  max2440bytes, hostXCR07; `research/runs/0.3.31-full-xstate-layout.log`.
  Báo cáo `research/EXTENDED-XSTATE-AUDIT-0.3.32.md`.
- 0.3.32 chọnphysicalsubset x87/SSE/AVX, completeAVX512group, PKRU; validatetheo
  CPUID.D offsets/sizes, khôngoverlap/supervisor/oversize, end<=3968. Tailprivate3584
  cộnglegacy512, sharedABI vẫn512. HostXCR0 là hợp lệsubsetphysical, không cốđịnh7.
- GIF0 mởtạm XCR0physicalmask trướcXSAVEhost, restoretoàn bộguest, setguestXCR0;
  exit mởlạimask, saveguest, restoretoàn bộphysicalhoststate vàexacthostXCR0
  trướcSTGI. Bao gồmcomponenthostđangdisable, tránh bỏrơiZMM/PKRU khi guest mởrộng.
  Hostbuffer/stack làsupervisormapping; khôngdereferencehostuserpointer trongcửa sổPKRUguest.
- PKEfastcode64 chỉkhiphysicalPKU vàPKRUstateđượcswitch. PKRUgiữ ngaycảguest
  XCR0[9]/OSXSAVEoff. Supervisor/XSS/XFD/AMX/CET/unknownlargerstate vẫnchặn.
  CPL3/APIC/SMP vàfullinvalid-XSETBV exceptionABI chưa mở.
- Unitlayout/XCR0dependencies/PKEpolicy/VMXmask/tailboundsresetPASS; bốnactualFPvariants
  PASS1000mutations với dynamicmask7. Chưacoiuserlandtestmask7 là proofAVX512/PKRU.
- xsaveprobe mới coverZMM0/ZMM31/K7, legacyrewrite, full-layoutguestXSAVE,
  nativePKRU/read-writekeyfault/recovery, disabledguestXCR0/OSXSAVE, PKRUreset;
  bytes vàcompiledtransition đãrà. Cácbài hardwaremới pendingloaded0.3.32.
- Ràlại70PE firmware, hashkhôngđổi; scannermởrộngPKRU/ZMM/opmask khôngdecodedsite.
  `research/efi-feature-inventory-0.3.32-25G332.json`. LinearPEscan khônglàreachability;
  TE/raw/dynamiccode vẫnngoài phạmvi.
- Offline0.3.32 PASS khôngwarning/imports339/ASan/UBSan/fourFPvariants/layoutunit/
  resolver6/scriptsyntax; `research/runs/0.3.32-offline-checks.log`.
- Sau reboot: gate thường vCPU --modes → VM --rom → fast-test, chỉkhiPASS mới
  defaultLinux khôngfeatureclear. CPL3 là điểmchặn kếđãbiết, khônghứaLinuxhoàn tất.

- Cài0.3.32 PASS: AuxKC đúngversion/path; installscript exit0, kmutil exit28
  yêu cầu reboot. `research/runs/0.3.32-install.log`. Loaded vẫn0.3.31;
  takeover/vms/fast/hv_support đều0. Backup0.3.31 tại
  `research/kext-backups/AMDHV-0.3.31.kext`. Dừng ở bước cần reboot.

## 0.3.32 hardware PASS và CPL3 0.3.33 (2026-11-11)

- Loaded0.3.32; gate thường vCPU --modes / VM --rom / fast-test đềuPASS,
  gồm AVX-512 ZMM0/ZMM31/K7, full-layout XSAVE, PKRU/PKE, reset/isolation và
  hostFP/XCR0restore. Logs `research/runs/0.3.32-{vcpu-test,vm-test,fast-test}.log`.
- DefaultLinux không featureclear đạt1130bytesconsole, rồi validator chặn
  re-entry CPL3 tại7fb6ebdb3125, CS33/a0fb, SS2b/c0f3, XCR0=e7; fail0.446s.
  `research/runs/0.3.32-vz-linux60.log`. Chưa Linuxboot hoàn tất/container.
- Rà nhóm CPL, segmentcache, SYSCALL/SYSRET/SWAPGS, IRET, INTn/DPL, TSS RSP0/IST,
  privilegedintercepts, XSETBV, IOPL/IOPB và userpage/SMEP/NPT trước khi cài.
  Báo cáo `research/CPL3-AUDIT-0.3.33.md`; firmwarehash không đổi/70PEinventory;
  audit executableELF của musl/libcrypto/busybox, không suy process từ ASLRRIP.
- 0.3.33 mở fast64 CPL0/3 có cache nhất quán/TSS, explicit VMCB.CPL mỗientry,
  publish SS.DPL theo actualCPL. Native INTn checksDPL; userXSETBV injectGP hoặc
  directbitmapexit, không commitXCR0; guard interceptđặcquyền; sửa userfetch/SMEP.
  Step/legacy/rings1/2/usernullSS/LDTselector/IOPL vẫn chặn.
- Offline0.3.33 PASS: build/imports339/ASan/UBSan/CPL+userfetchunits/fourFPvariants/
  resolver6/scriptsyntax; `research/runs/0.3.33-offline-checks.log`.
- Newcplprobe build/sign/staticdisassembly PASS; hardware pendingreboot:
  IRET3/re-entry/TLS/SYSCALL/SYSRET/SWAPGS/GP/UMIP/XSETBV/IOPB/userPF/NPT/
  IRQ/IST/INT80/DPLdenial/INT3/stackNPFvectoring vàcleanup.
- Sau reboot: gate thường vCPU --modes → VM --rom → fast-test (cplprobe cuối),
  chỉ khi toàn bộPASS mới defaultLinux. APIC/SMP/fullNMI/debug/compatuser32/
  interruptedtype4INTreinjection và containerlifecycle chưa hoàn tất.

- Cài0.3.33 PASS: installscript exit0, kmutil exit28 yêu cầu reboot; AuxKC đúng
  version/path. `research/runs/0.3.33-install.log`. Backup0.3.32 tại
  `research/kext-backups/AMDHV-0.3.32.kext`. Loaded vẫn0.3.32, takeover/vms/
  fast/hv_support đều0. Dừng ở bước reboot để kích hoạt và test hardwaremới.

## 0.3.33 hardware PASS và speculation controls0.3.34 (2026-11-11)

- Loaded0.3.33; vCPU --modes630 / VM --rom29 / fast-test172 PASS lines,
  khôngFAIL. CPL3/TLS/SYSCALL/SYSRET/TSS/IST/INTn/DPL/NPT đãhardwarePASS.
  Logs `research/runs/0.3.33-{vcpu-test,vm-test,fast-test}.log`.
- Sửa2lỗi userspacefixture trên cùngkext: HV_VM_DEFAULT và STAR userselectorRPL3
  theoAMD, không thaykernel/normalizationđểPASS. Loggốc giữfixture-options/star.
- DefaultLinux chạy /init và fork/SYSCALL, nhưng Oops switch_mm_irqs_off+4a9:
  WRMSR ECX49 EDX:EAX1 (PRED_CMD/IBPB). Lifecycle60sPASS nhưng guestfault,
  console5157bytes. `0.3.33-vz-linux60.log`; KHÔNGcoi làboot thành công.
- Diagnosticmitigations=off 10s tới AlpineInit/loaddrivers/emergencyrecoveryshell
  do không cóbootmedia: `0.3.33-vz-diag-mitigations-off.log`. Khônglàdefaultproof.
- vzprobe nay phát hiệnOops/Kernelpanic/pagefault từconsole và returnfailure;
  replaydefault1s đúngexit1: `0.3.33-vz-linux1-consolecheck.log`.
- Rà nhómIBPB/SBPB/SPEC_CTRL/IBRS/STIBP/SSBD/PSFD/L1D_FLUSH/legacyVIRT_SSBD/
  EFER.AUTOIBRS/modelMSRs, physicalCPUID, kernelELF và91MSRsitesfirmware70PE
  hashkhôngđổi. `research/SPECULATION-AUDIT-0.3.34.md` ghi phạmvi vàgiới hạn.
- 0.3.34 nativefast64: capability-gated realPRED_CMD/L1Dcommands; SPEC_CTRL
  guestprivateVMCB6e0, hardwareORhost, không cài guestvalue lênhost; hostvalue
  được đối chiếu sauquantum; GPcho reserved/unavailable/write-onlyreads,
  bitmap/nativepipeline, hardwareNRIP trướcsideeffect, state/re-entry/reset.
  Chưa mởmodel-specificMSRs/AUTOIBRS/legacyVIRT_SPEC_CTRL hoặcbitskhôngbiết.
- Offline0.3.34 PASS khôngwarning: build/imports339/ASan/UBSan/MSRcapabilityunits/
  fourFPvariants/resolver6/scriptsyntax. `research/runs/0.3.34-offline-checks.log`.
- Newspecprobe signed/staticbuildPASS; hardwarepending0.3.34. Sau reboot:
  vCPU --modes → VM --rom → fast-test (specprobe cuối), chỉ nếu tấtcảPASS mới
  defaultLinux khôngmitigationsoff. Chưa normalrootfs/container/APIC/SMPgoal.

- Cài0.3.34 PASS: installscript exit0; kmutil exit28 yêu cầu reboot;
  AuxKC đúng0.3.34 tại/Library/Extensions/AMDHV.kext.
  `research/runs/0.3.34-install.log`. Backup0.3.33 tại
  `research/kext-backups/AMDHV-0.3.33.kext`. Loaded vẫn0.3.33;
  takeover/vms/fast/hv_support đều0. Dừng tại reboot, hardwaremới chưa test.

## 0.3.34 hardware PASS, first diagnostic containers,0.3.35 EFI/memory (2026-11-11)

- Loaded0.3.34; gates vCPU --modes630 / VM --rom29 / fast-test203 PASS lines,
  zeroFAIL, gồmSPEC_CTRL/IBPB/SBPB/L1D/hostrestore. Logs0.3.34-{vcpu-test,vm-test,fast-test}.
- DefaultAlpine khôngfeatureclear/mitigationsoff tớiInit/emergencyrecoveryshell.
  vfkitinteractive thực thiuname/id/25forkexec/ghi+băm1MiB rồiguestpoweroffsạch.
  `research/runs/0.3.34-vfkit-linux-shell.log`. Chưa installedrootfs/defaultproviderproof.
- CloneAPFS riêng từstoppedPodmandisk, EFIvars riêng; sourceinode165146/mtime1793953935
  khôngđổi. EFI2GiB bịcap pin1GiB; EFI512MiB tớiGRUB CPUID rồi validatorchặn
  RFLAGS.ID200202. GRUBhelper10da9 thực sựtoggleID và giữnó, CPUIDRVA10e35.
- Rà đủpinbudget/NPTnodecap/partialunmap/rollback/globalaccounting; rerun70PEfirmware
  inventoryidentical vàscanthêmBOOTX64/GRUB. `research/EFI-MEMORY-AUDIT-0.3.35.md`.
- DIAGNOSTICONLY: tạm sửahelperGRUB trênclone đểrestoreID, khôngsửakext/gatethường.
  512MiB thiếuinitramfs/libsystemd;896MiB bootFedoraCoreOS44, kernel7.2.7 tới
  multi-user.target/Ready/SSH running, dùngNAT/RNG/vsock1025 và3disposableshares.
- FIRSTDIAGNOSTICCONTAINERS: MacPodmanCLI5.8.9 → explicitSSHengine5.8.9 trong
  vfkit/AppleVZguest, khôngQEMU. Alpine chạy; DNS/defaultbindreadwrite/volume giữa
  containers/buildimage+run PASS riênglẻ. PublishedportFAIL thiếugvproxygateway
  vớiNATfixture; workloadexit126. Chưađạtdefaultpodmanmachine/SMP/Dockergoal.
  `research/runs/0.3.34-macos-podman-diag-workload.log` vàbáocáođầyđủ.
- Guestpoweroffsạch; diagnosticGRUB đãrestoreoriginal/hashmatch. Clonegiữnamedvolume
  fixvm-diagnostic-data vớiFIXVM_VOLUME_PERSIST_OK đểtestpersistence sau normalboot.
- 0.3.35 preserve/admitID choallPilotpaths, vẫnchặnTF/IOPL/NT/VM/VIF/VIP;
  perVMpreparedpins8GiB/global16GiB underexistinggate, chargewholepreparedregion,
  commit/release/rollback đúng; NPTnodeceiling8192. Khônglazy/hugemapping/SMPmới.
- Offline0.3.35 PASS khôngwarning/imports339/ASan/UBSan/actualmemory2x8GiBstubmaps/
  modeID+reservedflags/fourFPvariants/resolver6/scriptsyntax. Newmemoryprobe>4GiB,
  step16/32/64 vàfastIDtoggle/re-entry hardwarepending0.3.35.
- Sau reboot gates vCPU --modes → VM --rom (memoryprobe) → fast-test; nếuPASS
  bootcloneGRUBgốc2GiB1CPU rồitestvolume vàtiếpapplehv/gvproxy/SMP/Docker.

- Cài0.3.35 PASS: installscript exit0, kmutil exit28 yêu cầu reboot; AuxKC đúng
  version0.3.35/path. `research/runs/0.3.35-install.log`. Backup0.3.34 tại
  `research/kext-backups/AMDHV-0.3.34.kext`. Loaded vẫn0.3.34; takeover/vms/
  fast/hv_support đều0, khôngvfkitVM/loopdiskcònchạy. GRUBcloneđãrestoregốc;
  sourcePodmandiskinode/size/mtime khôngđổi. Dừng ởreboot đểtesthardware0.3.35.

## 0.3.35 native Podman PASS; 0.3.36 cache/AP audit (2026-11-11)

- Loaded0.3.35: normal vCPU --modes / VM --rom / fast-test đều PASS, gồm >4GiB
  memory và IDflags. Logs `research/runs/0.3.35-{vcpu-test,vm-test,fast-test}.log`.
- Native Podman applehv/vfkit+gvproxy, GRUB gốc, 1CPU/2GiB: machine start/info/stop,
  container/DNS/virtiofs bind read-write/volume persistence/build-run/localhost18080
  nginx/API _ping OK. `research/runs/0.3.35-native-podman-workload-v3.log` PASS.
  Profile XDG và disk/EFI/shares clone riêng; chưa clean machine init. Build dùngcache.
  Global Docker socket alias vẫn trỏ defaultmachine cũ, phát pingwarning; actual
  isolatedAPI OK. Chưa DockerDesktop/SMP/performance hoặc fullgoal.
- Workloadv1/v2 fail do fixture httpd thiếu/existingvolume; giữ log, sửa fixture v3.
- SMP2 EFI AP #GP CSbase86000/IP10000 chưa rõnguyênnhân. DirectLinux SMP2 chặn
  cụthể WBINVD exit89 tại AP IP1/NRIP3. Hai lỗi chưa được chứngminh cùngnguyênnhân.
- AP slot1/separatethread/relativeTSC synthetic probes PASS trên35: realmode nonzeroCS,
  segments/CR0/GDT/IDT/PE/farjump32. Không chứngminh INIT/SIPI/fullSMP.
- Rà nhóm firmware/cache/AP và inventory70PE trong
  `research/CACHE-SMP-AUDIT-0.3.36.md`; không suyđoán BSPexit41 là TFleak.
- 0.3.36 consume WBINVD/WBNOINVD coherentRAM/noassignedDMA với MFENCE,
  validatedNRIP, shadow/RFretirement và boundedquantum; INVD forwardVMX13,
  exactlength/RIPunchanged. Không physicalhostcacheflush, secondarycontrolsvẫnreject.
  Thêm firstAPentrybytes/state diagnostic và unsupportedexitfullstate.
- Offline36 PASS imports339/ASan/UBSan/fourFPvariants/resolver6/scripts; compiled
  kernel khônghost INVD/WBINVD. Cacheprobe16/32/64, CPL3GP, CLI;WBINVD APfixture
  đãbuild/sign, hardwarepending36. Sau reboot chạy lại đủ3gates trước integration.

- Cài0.3.36 thànhcông: install exit0, kmutil exit28 yêu cầu reboot; AuxKC đúng
  0.3.36 tại /Library/Extensions/AMDHV.kext. `research/runs/0.3.36-install.log`.
  Backup35: `research/kext-backups/AMDHV-0.3.35.kext`. Loaded vẫn0.3.35;
  takeover/vms/fast/hv_support đều0, khôngvfkit/gvproxy/VM cònchạy. Isolatedprofile
  đã trả về1CPU/2GiB vàstopped; sourcePodmandisk inode165146/size99857989632/
  mtime1793953935 khôngđổi. Inventory36 identical35/70PE. Dừng ởreboot;
  hardwarecache36, SMP và DockerDesktop cònpending.

## 0.3.36 hardware/native PASS; 0.3.37 AP LME preparation (2026-11-11)

- Loaded36: gates vCPU --modes669 / VM --rom37 / fast-test240 PASS lines,
  zeroFAIL, exit0; cache16/32/64/CPL3GP/AP WBINVD/EFI APprefix PASS.
- Normal isolated nativePodman1CPU/2GiB workload PASS trên36, gồm build --no-cache,
  persistence sau hostreboot, bind/DNS/localhost18080/API _ping và machine stop.
  `research/runs/0.3.36-native-podman-workload.log`. Chưa cleaninit/globalalias/
  DockerDesktop/performance/fullgoal; khôngQEMU.
- Cả directAlpine SMP2 và PodmanEFI SMP2 vượt cache/firmware tới LinuxAP, dừng
  cùng EFER901/CR050033/CR420b0 hoặc20f0/RIP9a05d. Bộvalidator chỉ cho SCE
  activeIA32e nên chặn trước MOV CR0.PG. SMP vẫnFAIL. EFI #GP35 chưa táihiện,
  chưa kếtluận cănnguyên. Logs36-vfkit-linux-smp2 và36-native-podman-smp2.
- Rà actualAlpineELF trampoline16/32/64, CR/PAE/EFER/PG/compatfarjump, cache,
  CpuDxe/GDT/IDT; inventory70PE37 identical36. Báocáo
  `research/AP-LONGMODE-AUDIT-0.3.37.md` vàlinux/aptrampolineaudit.
- 37 cho SCE ở preparedLME ring0 chỉ trongfast; vẫnrejectstep/missingPE/PAE/
  barelegacy/invalidEFER. KhôngthayhostEFER, locks, FP/faultpolicy hoặc VMXABI.
  New longtransitionprobe --sce kiểm EFER901→d01/PG/compat64/TSS/NPT, addedfastgate.
- Probe mới trênloaded36 táihiệninitialreject (negativeexit1), nhưng explicitPG/LMA
  sauđó vẫnfarjump64/marker/HLT/NPT PASS; giữlog36-sce-preparation-negative,
  khôngcoi toànprobePASS. Hardware37 cònpending.
- Sửa diagnostic APentry16byte vốnluônbị helper15bytereject: nayread15/label15,
  khôngsuy unreadable36 là guestmappingfault. Khôngthayguestexecution.
- Offline37 PASS build/sign/imports339/ASan/UBSan/fourFPvariants/resolver6/scripts;
  sourcePodmandisk inode/size/mtimekhôngđổi, isolatedprofile đãtrả1CPU/2GiB/stopped.

- Cài37 PASS: installscript exit0; kmutil exit28 yêu cầu reboot, AuxKC đúng0.3.37
  tại /Library/Extensions/AMDHV.kext; codesign verify PASS. Backup36 tại
  `research/kext-backups/AMDHV-0.3.36.kext`, log `research/runs/0.3.37-install.log`.
  Loaded vẫn36, takeover/vms/fast/hv_support đều0. Dừng tại reboot để kíchhoạt37;
  chạy lại3gates (fast gồm --sce) rồi mới directLinux/Podman SMP2. Không tựreboot.

## 0.3.37 hardware and SMP integration (2026-11-11, ongoing)

- Loaded37; all three gates PASS, including prepared EFER901→d01 transition,
  WBINVD/AP slot1 and readable first-AP diagnostic. No new kext staged yet.
- Direct Alpine SMP2 boots to /init/recovery shell (no root media fixture), CPUonline0-1,
  processorcount2, both LAPIC timers/IPIs/TLBshootdowns, four parallel8MiB hashes match,
  guestpoweroff clean. `research/runs/0.3.37-vfkit-linux-smp2.log` exit0.
- Normal Podman applehv/vfkit SMP2: CLI/engine5.8.9, container/DNS/bind/volume
  persistence/uncachedbuild/localhost18080/actualAPI PASS, normalstop/idle0.
  `research/runs/0.3.37-native-podman-smp2-workload.log` exit0.
- Fresh Podman init fixvm-clean4CPU/4GiB from original cached base image PASS init;
  first boot NOT PASS: Podman Desktop auto-started original default12CPU concurrently.
  Clean guest reported HPET watchdog/softlockup; backend rejected NMI entry80000202.
  Original start canceled and Stop→HardStop through vfkit REST; Podman Desktop quit
  to prevent repeated auto-start. Original raw inode/size/mtime still unchanged.
- Retrying same interrupted cleanimage alone failed Ignition useradd group.lock left
  from interruption. Preserve both consolelogs and failed profile; not a cleaninitPASS.
  `0.3.37-clean-smp4-{concurrent,retry}-console.log`; workloadlogs retain exit125.
- Installed official Docker Desktop amd64 4.99.0/build245822, CLI29.9.3; archive/hash
  research/assets/Docker-amd64.dmg(.sha256), app codesign verify PASS. Accepted installer
  license; initial settings backed up, AutoStartfalse, 2CPU/4096MiB/VZ/virtiofs and
  disposable share only. No prior Docker app/data (containerdir4KiB).
- Docker native engine linux/virtualization-framework ready ~5.3s, actual
  com.docker.virtualization process2CPU/4096MiB + AppleVZ XPC; normalstop exit0,
  backendidle restored. `research/runs/0.3.37-docker-native-start.log`.
  Workload/restart/hostreboot acceptance still pending; no fullgoal claim.

## 0.3.37 native Podman/Docker milestone; 0.3.38 NMI (2026-11-11)

- Gates37 exit0:669/37/255 PASS lines, zeroFAIL. DirectLinux SMP2 CPUonline0-1,
  timer/IPI/TLBshootdown, four8MiB hashes and guestpoweroff PASS.
- Podman clone SMP2 full workload PASS; fresh machine init fixvm-clean2 2CPU/4GiB/
  disk16GiB from pristinecachedimage + Ignition + fullcontainer/DNS/bind/volume/
  uncachedbuild/localhost/API/start-stop PASS exit0. Chưa hostrebootproof mới.
- DockerDesktop4.99.0/build245822 installed officialamd64, CLI/engine29.9.3,
  actual AppleVZ/com.docker.virtualization, Linux7.0.14. Native2CPU fullworkload
  PASS; native4CPU strict fullworkload + CPU0..3 affinity32MiB hashesallmatch,
  normalstop exit0. `0.3.37-docker-native-smp4-strict.log`. NoQEMU.
- Dockervolume survived engine restart; newPodmanvolume proof read afterrestart,
  nhưng fullrepeat bịhủy exit143 do latency nặng/extraDockerstartup, khôngfullPASS.
  Earlytaskset fixture false completion corrected; 0.49/3.13s là INVALID CPUresults;
  strict4.70s gồm APK/network/container overhead, khôngCPUbenchmark.
- Docker4 startup33–52s vs2CPU~5.3s; timing/concurrency/stability vẫnopen.
  Podman4EFI GP/PF compatibilitymode RIPbfa55064/505d/CR3bfc01000 hoặc0 unresolved.
  Không coi lỗiEFI đãfix. Fresh4concurrentfirstboot rejected NMI80000202; retry
  interrupted disk bịgroup.lock, giữfailedimages/logs. PodmanDesktop auto-started
  default12CPU đãdừng/quit; originalraw inode/size/mtime vẫnunchanged.
- Harness now isolates runtime TMPDIR too: XDG-only shared gvproxy.pid withdefault
  machine. Newshortprivate tempdir avoids socketlength collisions; syntaxPASS,
  integrationpendingnextcycle. DockerAutoStartfalse, explicitengine sockets.
- 38 explicit NMI entry onlyfastnative64 + BOTHphysicalvGIF16/vNMI25; VINTRvGIFopen/
  vNMI, NMIblockingbit12↔VMXbit3, IRET hardwareunblock, IRQallowedwhileNMIblocked.
  No pendingqueue/NMIwindow/32modeNMI/newhostIRQ/scheduler or FPpolicychanges.
  Newnmiprobe + ASanstateunits; hardwarepending38. Morecode/GPR/farptr/GDT diagnostics
  forEFI fault, no guestrepair. Broadaudit70PEidentical and NMI/IRQ/IRET/vGIFgroup:
  `research/NATIVE-NMI-AUDIT-0.3.38.md`.
- Offline38 build/sign/imports339/ASan/UBSan/fourFPvariants/resolver6/scripts PASS.
  Newhardwarecases pendingreboot; fullprojectgoal NOT complete.

- Cài38 PASS: install exit0, kmutil exit28/reboot required, AuxKC đúng0.3.38 tại
  /Library/Extensions/AMDHV.kext, codesignverify PASS; backup37 tại
  research/kext-backups/AMDHV-0.3.37.kext. Loaded vẫn37. takeover/vms/fast/hv_support
  đều0; khôngvfkit/Dockerbackend/PodmanDesktop cònchạy. Clean2 đãtrả2CPU/4GiB/stopped;
  Docker4CPU/4GiB/AppleVZ/virtiofs/AutoStartfalse. Khôngtựreboot.
- Điểmnối `research/REBOOT-CHECKPOINT-0.3.38.md`: sau kíchhoạt38 chạyđủ3gates/NMI,
  read-only exactvolumeproof TRƯỚC workload ghi, rồi từngbackend riêng vàEFI4faultdump.
  Chưa hardwareNMI38, chưa hostrebootproof mới, chưa fullgoal/performance/stability.

## 0.3.38 hardware NMI PASS (2026-11-11, ongoing)

- Loaded38, initialidle0. Gates vCPU --modes669 / VM --rom37 / fast-test267 PASS
  lines, zeroFAIL, exit0. New NMI IF0/one-shot/blocked nested rejection/IRETQ/second
  NMI/IRQinsideNMI/IRQIRET unblocking all PASS on hardware.
  `research/runs/0.3.38-{vcpu-test,vm-test,fast-test}.log`.
- Docker4CPU/4GiB actualAppleVZ engine + read-only namedvolume proof afterhostreboot
  PASS exact FIXVM_DOCKER_PERSIST_OK before any write; cachedimage --pull=never,
  volumeinspectfirst. Normalstop exit0, gatesrestored0.
  `research/runs/0.3.38-docker-reboot-volume.log`.
- Podman clean2 read-only proof and EFI4 diagnosis continuing, fullgoal stillopen.


## 0.3.38 verified; staged0.3.39 (2026-11-11)

- Podman clean2/2CPU/4GiB read-only exact hostreboot volume proof PASS before writes,
  normalstop exit0. Both Podman and Docker now pass hostreboot persistence.
  `research/runs/0.3.38-podman-reboot-volume.log`.
- First defaultPodman4 run reachedLinux, TSC watchdog226495ns HPET vs197440ns TSC
  switched toHPET, rootUUID wait. Docker auto-opened11:21:25 and overlapped; CLIstop
  timedout then identifiedbackend TERM stoppedDocker. Podman canceled/hardstopped,
  gates0. This is NOT clean isolated default4 acceptance and EFI fault notprovenfixed.
  `0.3.38-podman-efi-smp4*` logs/console retain evidence.
- DiagnosticONLY: rpm-ostree queued tsc=reliable in disposable clean2,4CPU provider
  booted, currentclock=tsc. Prior exactreadonlyproof + fullcontainer/DNS/bind/volume/
  uncachedbuild/localhost/API and allCPU0..3 strictaffinity hashes PASS, normalstop,
  exit0. `0.3.38-podman-tsc-compare4.log`.6.33s includespackage/network/containeroverhead,
  notCPUbenchmark. This supports clock investigation, not a final default compatibility claim.
- Removed diagnosticargument beforeworkload; then booted2CPU default and verified
  BOTH /proc/cmdline and queued rpm-ostree args lacktsc=reliable. Exactproof/normalstop
  PASS exit0, `0.3.38-podman-default-restored.log`. Clean2 left2CPU/4GiB/stopped.
- HostinvariantTSC CPUID80000007.EDX6799;240000 serializedreads/24threads overAPIC
  bitmapfff0fff no backwards.4391.727MHz againstmachclock, whichmayitselfderiveTSC;
  notindependentfrequency/guestSMPskewproof. `0.3.38-host-tsc-sample.log`.
- Newaplongprobe:3independentAPslots1..3/threads+keeper0,64transitions each through
  real->PE->PAE/LME/PG->indirectFF2Ffar64; alternatingNXE/SCE, fullmarker/CR0/3/4/EFER
  assertions. Finalhardware38run4PASS,exit0; actualfirmware/sharedexchange race remains
  unproven. `0.3.38-aplong-test.log`; includedin39fastgate.
-39: sample successfulfast exits(first32/powers2), alwayslogexception/shutdown and
  unsupporteddiagnostics; newatomic runtime debug.amdhv.trace_exits fulltrace switch,
  releasecounters. ExistingunboundedIOLog+logmutex observed inspinloop; benefit notyetmeasured.
  No clock/timer/lock/scheduler/FP/NMIexecution orassemblypolicychanges.
- Broadaudit re-extracted70PE inventory39 exactlyequals38; reviewedAP/segments/paging/
  TSC/PAUSE/cancel/logginggroups. `research/CLOCK-TRACE-AUDIT-0.3.39.md`.
  Offline39build/signature/imports339/ASan/UBSan/FPvariants/resolver6 PASS exit0.
  FinalAPbinary rebuilt/hardwarePASS; script syntaxPASS; disassembly onlyMFENCE,
  nohostINVD/WBINVD. Hardware39trace/timing stillpending.
- Install39exit0: kmutil exit28/rebootrequired, AuxKC version0.3.39 atLibraryExtensions,
  installedcodesignverifyPASS; backup38 `research/kext-backups/AMDHV-0.3.38.kext`.
  Loadedstill38. takeover/fast/vms/hv_support all0; no testengines left. Noautomaticreboot.
  Next: `research/REBOOT-CHECKPOINT-0.3.39.md`. Fullgoal remainsopen.


## Hardware39 verified; staged40 parallel experiment (2026-11-11)

- Loaded39 gates exit0: vCPU669 / VM37 / fast271 PASS lines, zeroFAIL. New trace
  switch and AP real->64/NMI hardware PASS. Logs research/runs/0.3.39-*-test.log.
- Docker4CPU/4GiB AppleVZ fullworkload + all4affinityhashes + exactreadonlyhostreboot
  volumeproof PASS, normalstop exit0. Engine ready16.436s, fullworkload23.47s,
  nestedaffinity5.13s includespackage/container/network overhead, notCPUbenchmark.
  No meaningful startup speedup established versus38's16.611s proof run.
  research/runs/0.3.39-docker-smp4.log.
- Podman clean2 DEFAULT4CPU/4GiB/no tsc=reliable: currentclocktsc atworkloadentry,
  exactreadonlyhostrebootproof + fullcontainer/DNS/bind/volume/uncachedbuild/port/API
  and strictCPU0..3hashes PASS, normalstop exit0. Workload40.37s excludesstartup;
  nestedaffinity7.08s includesoverhead. WatchdogHPET/remoteCPU timeouts stilloccur.
  research/runs/0.3.39-podman-default-smp4.log and console. Singlepass notstable.
- Immediate default4repeat stalledbeforeengine, seriallogempty, highLinuxRIP spin
  quanta, no otherVM/providerinterference observed. RequestedStop, then ownedvfkit
  RESTHardStop; intentionalcancel exit125 (vfkit unexpectedly0), cleanupgates0.
  Preserve 0.3.39-podman-default-smp4-repeat*; no secondPASS/EFIrootcauseclaim.
  Clean2 restored2CPU/4GiB/stopped. Originaldefault12CPU profile nottesttarget.
- Newsmpprobe39: two guests mutuallyset/waitflags, noPAUSE; BOTHfirstcallsCPUID
  overlap0/64/errors0 confirmsserializedbaseline. Activeguest data-pageprotect/
  unmap/remap8rounds + normalthread/pincleanup PASS. Finalprobeexit0.
  research/runs/0.3.39-smp-serialization-baseline.log.
-40 opt-in debug.amdhv.parallel=0 bydefault, changeonlytakeover0/vms0. GlobalRWgate:
  ONLY fast run traps0/1 take sharedlifetime/mappinglease; allstate/map/lifecycle
  mutators exclusive; step stays exclusive. No target pointers acrossunlock.
  AllvCPU/privateFP slots remainownedthread; mailbox atomic, lockorderunchanged;
  ASID1 CPUlocal/fullflush everyquantum, noNPTedit whileactive. ThreadSafeIOmemory
  descriptors forparallelreadonlyfetch; unexpectedfastIRET failsclosed insteadof
  stepPTE/framewrites. HostIRQ/asm/clock/FP/NMIexecutionpolicyunchanged.
- fast-test serial and --parallel modes testpolicyguards, hostIRQbounds, positive
  overlappingguesthandshake(onlyparallel), mappingwriterprogress andallregressions.
  Harnesses explicitly FIXVM_PARALLEL=0/1 beforetakeover, restoreflag; nowrequire40.
  Podman start records externalelapsedtime includingproviderreadiness. Cancompare
  bothmodes onSAME40 afteractivation with guestrestarts, noextra hostrebootfortoggle.
- Broadaudit70PE matches39 SHA/instructiongroups afternormalizingorder/path;
 5extractionpaths differ, firmwarebytesunchanged. AP/segments/paging/atomics/
  IRQ/NMI/IRET/TSC/hostswitch/lifetime/descriptor groups reviewed together.
  research/PARALLEL-RUN-AUDIT-0.3.40.md; normalizedcomparisonlog retained.
- Offline40 exit0: build/sign/imports345/ASan/UBSan/FPvariants/resolver6 PASS.
  Finalsmpprobe rebuilt/testedon39 andallscripts syntaxPASS. DisassemblyMFENCEonly,
  nohostINVD/WBINVD. Parallel40 hardware/benefit stillPENDING, nofullgoalclaim.
- Installed40 exit0, kmutil28/rebootrequired; AuxKC0.3.40 at/Library/Extensions,
  codesignverifyPASS, backup39 research/kext-backups/AMDHV-0.3.39.kext.
  Loadedstill39, takeover/fast/trace/vms/hv_support all0; noenginesleft. NoautoReboot.
  Continue research/REBOOT-CHECKPOINT-0.3.40.md: serialgates thenparallelgates before
  providers, exactproofbeforewrites, repeateddefault4/timing; restore2CPUifunstable.


## 0.3.40 VM validation and Docker handoff (2026-11-11)

- Loaded40, gates669/37/273/273 zeroFAIL, exit0, both before and after hardrestart.
  Parallel smpprobe64/64 overlapping first calls, mapping writer/teardown PASS;
  physicalselftest24/24. Two VM tasks simultaneously peak2, SMP63/64 and AP3slots
  each64/64, no guesterrors, teardown0VMs. All hardware logs retained.
- Docker4CPU/4GiB actualAppleVZ:8 completeworkload runs(2serial/6parallel) including
  exactreadonlyproof BEFORE writes, CPU0..3 hashes, build/DNS/network/ports/bind/
  volume/API. Warm comparison serial8.401s startup/13.97sworkload versusparallel
  1.853s/6.02s. Parallelstartup samples1.853..2.399s; bounded local measurements.
  Extra4container CPU-pinned stress256correcthashes/8GiB aggregate cachedreads,
  diskwrite/sync/read + exactpersistedproof PASS,6.62s includescontainer/APIoverhead.
- Podman clean2 default2CPU one fullPASS, default4CPU3consecutivefullcycles + one
  afterhardrestart PASS; no tsc=reliable argument. Clocktsc atworkloadentry and no
  collectedunstablefallback/softlockup/Oops pattern. FullstrictCPUhash/build/network/
  storage/port/API/readonlypriorproof and normalstop allPASS. Leftstopped4CPU/4GiB.
  Original12CPUprofile notqualified. CLIstart30.45..31.54s includesglobalAPIalias
  pingtimeout20s inupstream5.8.9; isolatedAPIvalidatedseparately. Notpureguestboottime.
- Harnessrepeat3 racedoldDockerbackend single-instance socket whilevmsalready0;
  canceled143 beforeVMstartup, notkernel/workloadPASS. Addedstartprocessguard and
  waitforbackendmonitor/servicesexit afterteardown. Repeat3retry/repeat4 andall
  recovery/warm/finalruns PASS, explicitshutdownmarker. HeadlessCLIstop sometimes
  timesoutbeforemonitorends; ownedlauncherTERM+processdrain completescleanup.
- Host IdleSleep12:10:22 interrupted original serialwarmcomparison; userhardrestart
  newboot13:17. That logNOTPASS; plannedparallelfinal/readydidnotexecute then.
  Savedpmset/unifiedpowerlogs; cause ofwakefailure notestablished. Userlatestasks
  DEFERsleep/waketesting, prioritizeVM. No sleeps/resume tests performed or planned
  in this turn. Temporarycaffeinate mitigatesidleSleepduringtesting, notafix.
- Re-ranall4gates and BOTH exactvolumeproofs/fullworkloads afterhardreset PASS,
  thencompletedwarmserial/parallelcomparison. Per-probe caffeinate -i -w PID now
  holdsassertionforprobelifetime, observedinpmset and releasedafterexit. No power
  settings change/kernelversion/reboot needed for mitigation or remainingtests.
- Newmanual scripts/desktop-backend.sh start/stop/status verifiedstart+stop; final
  start succeeded. Loaded/installed/source remain40. HandoffINTENTIONAL:
  fast1/parallel1/takeover1/hv_support1, trace0, vms0; noDocker/vfkit/PodmanDesktop
  running. User may openDockerDesktop GUI now, CLIcontextdesktop-linux. Testengine
  configured4CPU/4GiB/AppleVZ/virtiofs, disposableUsersshare, AutoStartfalse.
- User-level detached idle-sleep guardPID2664 started13:28:36 for7200s, observed
  inpmset, expires~15:28local; onlytemporarytrialguard. See user-trial-awake.log.
  Sleep/wake deferred, longtermstability/broaderconfigs/dailyactivation stillopen;
  nofullgoalcompletionclaim. Reports research/NATIVE-ACCEPTANCE-0.3.40.md,
  research/USER-DOCKER-0.3.40.md, research/SLEEP-WAKE-INCIDENT-0.3.40.md.

- Latest handoff update: normal Docker backend started13:29:29 while documentation
  was being finalized. Read-only API ping OK, engine29.9.3/4CPU/Desktop; cached
  ephemeral GUI-backend smoke container exit0 prints x86_64,4,FIXVM_DOCKER_GUI_OK.
  Currentvms1 is the running Docker instance, not an orphan from the probe. Leave
  it running for the user. fast/parallel/takeover/hv_support1,trace0; no reboot.
  User priority: defer sleep/wake, focus working VMs. Trialawakeguard2664 active.
