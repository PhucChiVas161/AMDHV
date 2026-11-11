# FixVM: nhật ký quá trình

Ghi lại theo thứ tự mọi việc đã làm trong dự án AMDHV. Kế hoạch tổng thể và chi tiết
kỹ thuật nằm ở [PLAN.md](PLAN.md); file này trả lời câu hỏi "đã làm gì, kết quả ra sao".

## Mục tiêu

Cho **Podman và Docker Desktop dùng được backend ảo hóa native của macOS trên
Ryzentosh**, ưu tiên Podman `applehv`/vfkit và backend Apple của Docker phù hợp phiên bản
cài đặt. AMDHV dùng AMD SVM để cung cấp API/ABI mà Hypervisor.framework và
Virtualization.framework cần. Linux container vẫn nằm trong máy ảo Linux của backend.

QEMU HVF boot Linux là mốc kiểm thử trung gian, không phải đích cuối. Đường sử dụng cuối
không phụ thuộc QEMU/Lima/TCG hay máy Linux từ xa. Tiêu chí hoàn tất cụ thể nằm trong
[PLAN.md](PLAN.md): cả Podman và Docker đạt container/build/network/storage/lifecycle,
ổn định và có số đo hiệu năng thực tế.

## Máy

| Mục | Giá trị |
|---|---|
| CPU | AMD Ryzen 9 9900X (Zen 5), 24 luồng, 64 GB RAM, SVM bật trong BIOS |
| macOS | 26.7.2 Tahoe (25G332), XNU 12377.161.15, x86_64 |
| Bootloader | OpenCore trên `disk0s5` (chung EFI với Limine của CachyOS) |
| SIP | `csr-active-config = 0x0A43` (kext chưa ký, kext chưa duyệt, FS không bảo vệ) |
| Ổ đĩa | Một ổ chia ba: Windows, macOS, CachyOS (dual-boot) |
| Công cụ | Command Line Tools (clang 21, SDK 26.5), KDK 25G332 |

---

## Nhật ký (2026-11-06)

### 1. Khảo sát máy
- Xác nhận `kern.hv_support = 0`, Podman có sẵn ở `/opt/podman` (kèm vfkit) nhưng không chạy được.
- Liệt kê kext đang nạp: Lilu, WhateverGreen, VirtualSMC, AMFIPass, NVMeFix, RestrictEvents,
  USBToolBox, RTL812xLucy, và bộ kext NVIDIA nullmoth (nạp từ `/Library/Extensions`).
- Phát hiện `com.apple.driver.AppleHV` có nạp trên máy AMD (là IOService gắn vào IOResources).

### 2. Đọc config OpenCore
- Người dùng mount hai phân vùng EFI bằng sudo. OpenCore nằm ở `disk0s5`.
- Đọc `config.plist`, bỏ qua `PlatformInfo` (serial). Có bộ patch AMD Vanilla (MaxKernel 25.99.99),
  `SecureBootModel = Disabled`, `Kernel.Block` trống, boot-args có `keepsyms=1 debug=0x100`.

### 3. Tìm hiểu Hypervisor.framework (phía userspace)
- Dùng `dyld_info` trên dyld cache: framework có khoảng 80 hàm export, **không import IOKit hay
  syscall**. Nó gọi xuống kernel qua Mach trap kiểu machdep (`hv_task_trap` / `hv_thread_trap`).

### 4. Tải và cài KDK
- Trang KDK của Apple cần đăng nhập, nên tải bản `Kernel_Debug_Kit_26.7.2_build_25G332.dmg`
  từ kho mirror `dortania/KdkSupportPkg` trên GitHub, lưu ở `~/Downloads/KDK_25G332/`.
- SHA256 khớp với checksum của kho; gói `.pkg` bên trong có chữ ký "signed Apple Software".
- Người dùng cài bằng `sudo installer`. KDK nằm ở `/Library/Developer/KDKs/KDK_26.7.2_25G332.kdk`.

### 5. Reverse engineering AppleHV (từ KDK, có symbol)
- AppleHV import từ kernel: `hv_set_traps`, `hv_release_traps`, `hv_set_callbacks`,
  `hv_set_task_target`, `hv_ept_pmap_create`, `host_vmxon`...
- Giải mã đủ **14 task trap + 14 thread trap** (bảng ở PLAN.md).
- Có khoảng 200 lệnh VMX inline trong 55 hàm, nên không thể giữ AppleHV và chỉ vá lệnh VMX.
- Phần giả lập APIC/IOAPIC/PIC có tên hàm trùng **bhyve** (FreeBSD, giấy phép BSD), nên có thể port.
- `hv_callbacks_t` là 8 con trỏ (0x40 byte), truyền theo giá trị.
- **Quyết định kiến trúc:** viết kext mới (clean-room) cùng ABI với AppleHV, chạy trên SVM.
- Disassembly lưu ở `research/AppleHV-25G332.s`.

### 6. Thay đổi kế hoạch theo yêu cầu
- Bỏ hướng "Docker dùng tạm" (VirtualBox, QEMU TCG).
- Bỏ hướng dựng máy ảo dev trên CachyOS.
- Phát triển và thử **thẳng trên máy thật**.

### 7. Giai đoạn 1: khung kext + self-test SVM
- Dựng dự án build bằng Makefile (không cần Xcode): `AMDHV/src/`, `AMDHV/Info.plist`, `Makefile`.
- Viết `svm_defs.hpp` (MSR, offset VMCB, exit code), `svm_vmrun.S` (clgi → vmrun → lưu GPR → stgi),
  `svm_selftest.*` (guest real mode `mov eax,0x12345678; cpuid; hlt` chạy trên mọi nhân qua
  `mp_rendezvous_no_intrs`, có NPT, chặn mọi I/O và MSR), `AMDHV.cpp` (IOService, ghi kết quả vào IORegistry).
- Build sạch, mọi symbol import đều được KPI export.

### 8. Sự cố: Tahoe không nạp kext ngay
- `kmutil load` lần đầu chỉ gộp kext vào **Auxiliary Kernel Collection** (AuxKC) và đòi reboot.
  Kext trong AuxKC **tự nạp mỗi lần boot**.
- Bản đầu (0.1.0) chạy self-test ngay trong `start()`. Nếu lỗi thì máy panic lặp lại mỗi lần boot,
  nên đã chặn không cho reboot với bản đó.
- Sửa thành 0.1.1: lúc boot chỉ đọc CPUID; self-test chỉ chạy qua `sudo sysctl debug.amdhv.selftest=1`;
  thêm boot-arg tắt khẩn cấp `-amdhvoff`.
- Hai lỗi build phụ: `IOLockLock` không được export (dùng `lck_mtx_lock` qua `IOLockGetMachLock`),
  và hàm `lock()` trùng tên (đổi thành `amdhvLock()`).

### 9. Sự cố: AuxKC không được build lại
- Người dùng reboot nhưng `debug.amdhv.selftest` không tồn tại. AuxKC vẫn ghi bản 0.1.0 ở đường dẫn
  cũ `/private/var/tmp/fixvm` đã bị xóa, nên không có AMDHV nào chạy (máy an toàn).
- Nguyên nhân: script nuốt lỗi của `kmutil`. Sửa `install.sh` để in lỗi, tự gọi `kmutil rebuild`,
  cuối cùng in OK/FAILED.
- Chạy lại: kext cần **duyệt trong System Settings**. Sau khi duyệt, AuxKC có bản 0.1.1.
- Thêm cờ `kmutil load -z` (bỏ qua bước duyệt) vào `install.sh` cho các lần sau.

### 10. Kết quả giai đoạn 1: PASS 24/24
- Self-test VMRUN đạt trên cả 24 nhân: exit CPUID (0x72) rồi HLT (0x78), guest RAX = `0x12345678`.
- `VM_CR = 0x8` (BIOS khóa, SVM bật). SVM rev 1, 32768 ASID.
- Tính năng: NPT, LbrVirt, SVML, NRIPS, TscRate, VmcbClean, FlushByAsid, DecodeAssists,
  PauseFilter/Threshold, AVIC, x2AVIC, VirtVmsaveVmload, vGIF, GMET, vNMI.
- Sửa lỗi hiển thị `SVM Feature Bits` (ioreg hiển thị số 32-bit theo kiểu có dấu).

### 11. Chuẩn bị giai đoạn 2
- `AppleHV::start` **không kiểm tra CPU**: luôn đăng ký trap, kể cả trên AMD. Chỉ cần
  `hv_release_traps` rồi đăng ký trap của AMDHV, không phải chặn AppleHV trong OpenCore.
- `kern.hv_support` là `SYSCTL_INT` trỏ vào biến `hv_support_available`.

### 12. Giai đoạn 2: takeover trap (v0.2.0)
- Các hàm `hv_*` không có trong KPI cho kext bên thứ ba, nên viết `tools/gen_kernsyms.py` sinh
  `kernsyms.h` (địa chỉ + 24 byte đầu hàm, lấy từ kernel trong KDK) và `kernel_resolver.*`
  (kiểm tra version kernel và byte đầu hàm trước khi dùng).
- `hv_traps.*`: 28 trap stub, mỗi trap ghi tiến trình gọi, tham số, 64 byte bộ nhớ user tại `arg`,
  rồi trả `HV_UNSUPPORTED`.
- `klog.*`: ring log 64 KB, đọc bằng `sysctl -n debug.amdhv.log`.
- Sysctl mới: `debug.amdhv.takeover` (1 = nhận trap + `kern.hv_support=1`, 0 = trả lại), `debug.amdhv.log`.
- `tools/hvprobe/`: tool gọi Hypervisor.framework như QEMU (có entitlement `com.apple.security.hypervisor`).
- `tools/check_imports.sh` / `make check`: kiểm tra mọi symbol import đều được KPI export. Bản đầu
  báo sai do không đọc được symbol "indirect" và do lỗi `pipefail`; đã sửa.
- Chạy hvprobe khi AppleHV còn giữ trap: `hv_vm_create` → `HV_ERROR`. `hv_vmx_read_capability` trả
  SUCCESS mà không gọi trap: framework đọc capability từ hv commpage (việc của giai đoạn 3).

### 13. Sự cố: takeover bị kiểm tra an toàn chặn
- Chạy `scripts/trap-trace.sh` trên 0.2.0: `slide cross-check failed`. Kext không gọi hàm nào,
  trap vẫn do AppleHV giữ, máy không ảnh hưởng.
- Nguyên nhân: trong Boot Kernel Collection, phần dữ liệu của kernel không dời cùng khoảng với phần code,
  nên phép so địa chỉ `real_ncpus` sai.
- Lượt phân tích file KC để kiểm chứng bị bộ lọc an toàn dừng giữa chừng, chưa có kết quả.

### 14. Bản 0.2.1 (đã build, CHƯA cài)
- Bỏ phép so địa chỉ dữ liệu. Giữ kiểm tra version kernel và 24 byte đầu của từng hàm `hv_*`.
- Lấy `hv_support_available` hoàn toàn từ cây sysctl đã export.
- Build sạch, `make check`: imports OK (328 symbol).
- Phương án dự phòng nếu byte đầu hàm vẫn không khớp: lấy địa chỉ các hàm `hv_*` từ AppleHV đang
  chạy (AppleHV đã được kernel link sẵn tới chúng).

### 15. Cài bản 0.2.1 vào AuxKC
- Xác nhận máy vẫn nạp 0.2.0, takeover=0, kern.hv_support=0; log cũ báo `slide cross-check failed`.
- Chạy `make` và `make check`: thành công, imports OK (328 symbols).
- Chạy `sudo scripts/install.sh`: kmutil trả 28 (cần reboot), kiểm tra AuxKC xác nhận
  `com.fixvm.driver.AMDHV 0.2.1 /Library/Extensions/AMDHV.kext` và script báo OK.
- Chưa reboot, chưa thử takeover trên 0.2.1; không coi việc cài thành công là runtime PASS.
- Sửa ghi chú cleanup: `takeover=0` chỉ gỡ trap AMDHV và khôi phục hv_support,
  **không đăng ký lại trap AppleHV**. Reboot sẽ đưa AppleHV trở lại trạng thái ban đầu.

### 16. Thử 0.2.1 sau reboot; sửa relocation trong bản 0.2.2
- Xác nhận 0.2.1 đã nạp. `trap-trace.sh` bị chặn trước khi thay trap:
  `code signature mismatch: hv_release_traps`. Cleanup xong, takeover=0 và hv_support=0.
- Đối chiếu KDK với Boot KC: UUID kernel cùng là `28267fd5-3e1e-3686-8a1c-e73e2466fcfb`.
  Kernel __TEXT dời +0xe8000; __DATA dời khác. Các displacement RIP-relative tới dữ liệu
  trong `hv_release_traps`, `hv_set_callbacks`, `hv_release_callbacks` bị linker sửa.
  Ví dụ release_traps: `58 a7 ab 00` → `58 27 f2 00`. Năm hàm còn lại khớp 24 byte.
- Sửa generator lấy symbol từ KDK, kiểm tra UUID kernel nhúng trong KC, chuyển địa chỉ
  theo offset trong __TEXT, rồi lấy đủ 24 byte từ KC đã link. Không mask/bỏ kiểm tra byte.
  Bỏ các địa chỉ dữ liệu KDK không dùng; hv_support vẫn tìm qua sysctl.
- Thêm chẩn đoán offset/byte khi mismatch. Makefile phụ thuộc kernsyms.h để không dùng object cũ.
- Bump 0.2.2; `make kernsyms`, build, `make check` đều đạt (328 imports).
  6 regression tests đạt: đọc đúng file offset, từ chối UUID khác, địa chỉ ngoài text/padding,
  thiếu kernel, dữ liệu bị cắt và load command lỗi.
- Đã cài: AuxKC xác nhận `com.fixvm.driver.AMDHV 0.2.2 /Library/Extensions/AMDHV.kext`.
  Máy vẫn nạp 0.2.1, cần reboot; chưa xác nhận runtime takeover thành công.

### 17. Giai đoạn 2 PASS trên 0.2.2
- Sau reboot, xác nhận máy nạp 0.2.2. Resolver khớp byte của cả 8 hàm,
  slide=0x18a00000; cài thành công 14 task trap và 14 thread trap.
- `kern.hv_support` lên 1; hvprobe gọi task[0] vm_create với target=NULL,
  flags=0; trả HV_UNSUPPORTED đúng như stub thiết kế. Capability vẫn bằng 0.
- Cleanup thành công: takeover=0 và kern.hv_support=0. Không tạo VM thật.
- Thêm `hvprobe --expect-stubs`; trap-trace.sh kiểm tra kết quả probe, log vm_create,
  trạng thái sau cleanup và truyền lỗi ra exit status. Signal INT/TERM thoát qua cleanup.
- Build hvprobe và kiểm tra cú pháp zsh đạt; chạy lại end-to-end PASS.
  Output lưu tại `research/runs/0.2.2-phase2-verification.log`; log lần đầu tại
  `research/runs/0.2.2-trap-trace.log`.
- Bắt đầu phân tích ABI giai đoạn 3 tại `research/PHASE3-ABI.md`: vm_create copyin
  16 byte, output interface 0x118 byte; vm_map 0x28 byte; vcpu_create 16 byte,
  RW/RO stride 0xe18/0x6d8. Cần thay callback AppleHV trước khi gắn target AMDHV.
- Không đổi/cài kext ở lượt này; vẫn chạy 0.2.2, không cần reboot thêm cho các thay đổi userspace.

### 18. VM/NPT thật trong 0.3.0, PASS trên máy thật
- Thêm `hv_abi.hpp`, `vm_backend.*`, `guest_memory.*`, `npt.hpp`.
- vm_create dựng interface RW/RO/clock, kiểm tra entitlement và copyin/copyout;
  callback task exit thu hồi VM. Takeover thay callbacks AppleHV trước khi gắn target AMDHV.
- NPT 4 cấp, trang 4 KiB, map/unmap/protect; giữ backing pages bằng IOMemoryDescriptor,
  rollback khi lỗi, hỗ trợ partial unmap và remap lỗ. Giới hạn hiện tại 1 GiB pinned/VM,
  1024 trang bảng NPT, 16 VM; chỉ address space 0 và quyền NONE/R/RW/RX/RWX.
- Backend từ chối detach khi còn VM; không hỗ trợ hot-unload sau khi đã công bố raw trap pointers.
- `make test-memory` chạy chính mã quản lý bộ nhớ với IOKit giả lập dưới ASan/UBSan: PASS.
- Sau reboot vào 0.3.0, `scripts/vm-test.sh` PASS: entitlement, copyin/copyout lỗi,
  mapping RW/RO, duplicate create, 32 vòng create/destroy, partial map/protect/unmap,
  process exit, busy detach. Log: `research/runs/0.3.0-vm-test.log`.
- Kết thúc test: takeover=0, hv_support=0, vms=0. Đây chưa phải kiểm thử VMRUN qua framework.

### 19. vCPU và đường SVM giới hạn trong 0.3.1 (đã cài AuxKC, chờ reboot)
- Đối chiếu disassembly framework; thêm tool `tools/hvprobe/dump_abi.c` đọc bảng lookup
  trong mapping framework của chính process, sinh `vmcs_fields.hpp`. Không đọc process khác.
- `VcpuState` ánh xạ register/VMCS, valid/dirty mask; snapshot riêng trước khi kiểm tra
  đầu vào VMCB, không nhận giá trị exit giả do user sửa vùng RW.
- vcpu_create/destroy theo thread; callback thread-exit thu hồi vCPU, task-exit giữ VM
  tới khi không còn vCPU. vm_destroy từ chối khi còn vCPU.
- Thêm trampoline VMSAVE/VMLOAD để giữ FS/GS/TR/LDTR và syscall MSRs của host.
- Đường chạy ban đầu chỉ nhận real mode với trạng thái giới hạn, đặt TF + intercept #DB,
  tối đa 1024 bước/call; intercept POPF/IRET để guest không tắt step budget.
  CR0.TS/MP chặn FPU guest; intercept I/O, MSR, CR/DR, VM instructions và thao tác hệ thống.
  Không hỗ trợ protected/long mode, FPU, managed MSR, APIC hay boot Linux ở bản này.
- Flush ASID trước mỗi lần vào; dịch CPUID/HLT/NPF sang mã exit VMX cơ bản.
- `make check`: 339 imports OK; `make test-vcpu` và `make test-memory` PASS dưới ASan/UBSan.
- Đã cài vào AuxKC: `com.fixvm.driver.AMDHV 0.3.1 /Library/Extensions/AMDHV.kext`; script báo OK.
  Máy vẫn chạy 0.3.0, takeover=0, vms=0, hv_support=0; cần reboot trước test vCPU.
- `scripts/vcpu-test.sh` chạy lifecycle trước rồi guest ghi RAM → CPUID → HLT,
  unmap/NX fault và vòng lặp vô hạn có giới hạn. CHƯA chạy các test này trên kext mới.

### 20. 0.3.1 PASS VM/NPT và vCPU/SVM trên máy thật
- Sau reboot xác nhận loaded 0.3.1. Chạy lại VM/memory gate: PASS toàn bộ.
- vCPU lifecycle: tạo/hủy, alias register/VMCS, thread-exit và task-exit cleanup PASS.
- Guest real mode ghi RAM qua NPT, CPUID → HLT, unmapped/NX faults, bounded loop PASS.
- Logs: `research/runs/0.3.1-vm-test.log`, `research/runs/0.3.1-vcpu-test.log`.
- Kết thúc: takeover=0, hv_support=0, vms=0.

### 21. 0.3.2 mở rộng mode presets (chưa runtime-test)
- Thêm preset CPL0 protected32 không paging và long64 với 4-level paging; vẫn giới hạn
  1024 bước, chưa hỗ trợ FPU/interrupt/VMX controls cho Linux.
- Chuyển đúng segment attributes VMX bits 12..15 ↔ SVM bits 8..11;
  snapshot lại segment cache sau VMEXIT. Kiểm tra CR0/CR3/CR4/EFER và segment nhất quán.
- Thêm `modeprobe`: CPUID/HLT và ghi RAM 32/64-bit; long64 dùng VA 0x100000 → GPA 0x8000,
  tháo backing guest PT để xác nhận lỗi NPT khi page walk (PTE GPA 0x4800).
- `make check`: 339 imports OK. `make test-modes test-vcpu test-memory`: PASS ASan/UBSan.
- Chạy sau reboot: `sudo scripts/vm-test.sh`, `sudo scripts/vcpu-test.sh --modes`.
- AuxKC đã xác nhận `com.fixvm.driver.AMDHV 0.3.2 /Library/Extensions/AMDHV.kext`.
  Install exit 0; log `research/runs/0.3.2-install.log`. Cần reboot để kích hoạt.

### 22. Cập nhật mục tiêu theo yêu cầu người dùng
- Đích cuối: Podman/Docker Desktop dùng backend native macOS trên AMD, không phụ thuộc QEMU.
- QEMU HVF boot Linux giữ vai trò mốc kiểm chứng phase 3, không đủ để kết thúc dự án.
- PLAN đã đổi phase 4 sang Virtualization.framework/vfkit + Podman applehv;
  phase 5 sang Docker Desktop native backend và kiểm chứng ổn định/hiệu năng cả hai.
- Đây là thay đổi mục tiêu/lộ trình; không thay đổi kết quả kỹ thuật: loaded 0.3.1,
  AuxKC 0.3.2 đã staged, vẫn chờ reboot và runtime-test mode presets.

### 23. Kiểm chứng capability Virtualization.framework cho backend native
- Máy vẫn nạp 0.3.1, chưa reboot vào 0.3.2. Podman 5.8.9, vfkit 0.6.4;
  `podman-machine-default` provider applehv, 12 CPU, đang stopped. Không sửa/start máy này.
- Chưa có Docker Desktop trong /Applications; cần kiểm chứng/cài phiên bản x86_64 phù hợp
  ở mốc Docker, không suy diễn rằng đã chạy được Docker.
- Thêm `make vzprobe`, tool Objective-C dùng Virtualization.framework, entitlement virtualization.
  Tạo EFI variable store tạm riêng, validate cấu hình 1 CPU/512 MiB rồi xóa; không start VM.
- Baseline takeover=0: isSupported=false, VZErrorDomain 2, hardware unavailable.
- Takeover=1: isSupported=true, EFI configurationValid=true, errorCode=0.
  Đây chỉ là capability/configuration gate, chưa chứng minh start VM hay boot Linux.
- Script `sudo scripts/vz-probe.sh` tự cleanup takeover và kiểm tra vms/hv_support trở về ban đầu.
  Runtime PASS; logs `research/runs/0.3.1-vz-baseline.json`, `research/runs/0.3.1-vz-takeover.log`.
- Vẫn cần reboot vào 0.3.2 để tiếp tục mode hardware tests trước khi mở rộng guest execution.

### 24. Native EFI start trace và sửa host read-only mapping trong 0.3.3
- Mở rộng vzprobe `--start`: EFI VM tạm 1 CPU/512 MiB, không đĩa/network và không sửa
  Podman machine; chờ start/stop tối đa 15 giây mỗi bước. EFI store tạm được xóa.
- Trên 0.3.1, Apple XPC VirtualMachine gọi vm_create(flags=0), map RAM 512 MiB RWX,
  rồi ROM 1 MiB RX tại GPA 0xfff00000. Start trả VZErrorDomain 1 trước vcpu_create.
  Trace chưa có return code nên không khẳng định riêng log này chứng minh map ROM lỗi.
- `romprobe` riêng tái hiện: host mmap + mprotect(PROT_READ), guest RX → HV_ERROR
  (0xfae94001) trên 0.3.1. Log `research/runs/0.3.1-rom-test.log`.
- GuestMemory trước đó luôn pin InOut. 0.3.3 thử writable pin trước, fallback Out nếu
  guest không yêu cầu W; ghi nhớ quyền pin, từ chối vm_protect tăng W trên read-only pin.
  Thêm tests fallback, protect, failure rollback, cleanup và writable backing upgrade.
- Unit tests ASan/UBSan memory/modes/vcpu PASS, 339 imports OK; thêm trap return-code log
  để xác định chính xác API tiếp theo bị chặn. 0.3.3 bao gồm mode presets chưa runtime-test của 0.3.2.
- XPC cleanup đôi khi mất hơn 2 giây: lần đầu detach bị EBUSY, VM tự thu hồi sau đó và
  đã restore takeover=0. Script chờ tới 30 giây; chạy lại start-failure thu hồi sạch.
  Log `research/runs/0.3.1-vz-start.log`; không kill tiến trình Apple.
- AuxKC đã xác nhận 0.3.3 thay 0.3.2; install exit 0, log `research/runs/0.3.3-install.log`.
  Máy còn nạp 0.3.1, takeover=0/vms=0/hv_support=0. Cần reboot để test 0.3.3.

### 25. Runtime 0.3.3 và chuẩn bị 0.3.4
- Loaded 0.3.3 sau reboot. VM/memory + ROM RX và chống nâng W trên host RO: PASS.
  Log `research/runs/0.3.3-vm-test.log`.
- Real-mode/lifecycle regression PASS. Protected32/long64 chạy tới CPUID và ghi RAM đúng,
  nhưng resume HLT và lần page walk sau bị HV_UNSUPPORTED (4 assertions fail).
- Diagnostic dump: CS AR từ 0xc09b → 0x409b (32-bit), 0xa09b → 0x209b (64-bit);
  CPU bỏ G trong saved CS. Probe `--repair-cs` khôi phục G ở userspace thì HLT và
  page-walk NPF GPA 0x4800 đều PASS. Đây là diagnostic, không phải bản driver đã sửa.
- 0.3.4 dựng lại G từ effective segment limit > 0xfffff; test regression cho CS.G.
- Native EFI start trên 0.3.3 đã map RAM/ROM RX/region R thành công, rồi tới vcpu_create
  flags=2 và bị HV_UNSUPPORTED. Log `research/runs/0.3.3-vz-start.log`.
- SDK xác định flags=2 là HV_VCPU_TSC_RELATIVE. 0.3.4 nhận flag này, đọc offset từ
  RW+0xc78 khi dirty bit 0x100, dùng VMCB TSC_OFFSET và cho RDTSC chạy trực tiếp.
  Clock tuple (0,0) hiện tại khiến hv_tsc_clock bằng host TSC. RDTSCP/RDPMC, scaling,
  default-mode TSC update vẫn chưa hỗ trợ; không công bố capability rộng hơn.
- `tscprobe` kiểm chứng RDTSC guest nằm giữa hv_tsc_clock trước/sau cộng offset
  0/+1e9/-1e9. Chưa chạy được trên 0.3.4 trước reboot.
- Build không warning; imports 339 OK, memory/modes/vcpu ASan/UBSan PASS.
  AuxKC xác nhận 0.3.4, install exit 0; log `research/runs/0.3.4-install.log`.
  Machine hiện tại loaded 0.3.3, takeover=0/vms=0/hv_support=0; cần reboot.

### 26. 0.3.4 mode/TSC PASS; 0.3.5 managed MSR
- Loaded 0.3.4 sau reboot. VM/ROM, lifecycle/real mode, protected32/long64 resume,
  page-walk NPF và relative TSC offsets 0/+1e9/-1e9 đều PASS, không cần workaround CS.
  Logs `research/runs/0.3.4-vm-test.log`, `research/runs/0.3.4-vcpu-test.log`.
- Native VZ EFI start tạo vCPU flags2 thành công, tiếp theo thread[3] enable_managed_msr
  arg3 trả HV_UNSUPPORTED. XPC task thu hồi sạch. Log `research/runs/0.3.4-vz-start.log`.
- Tool `dump_msr.c` gọi pure lookup trong mapping framework của chính process:
  managed IDs1..5 = STAR/LSTAR/CSTAR/SFMASK/KERNEL_GS_BASE; arg3 là CSTAR.
- 0.3.5 thêm enable/disable cho 5 MSR này, private snapshot/cache dirty RW+0xc50,
  RO slot table +0x58 và entries16byte. Validate canonical address/32-bit SFMASK.
- MSR guest đều bị intercept. Với managed MSR hợp lệ, emulation cập nhật VMCB/private state,
  advance RIP và tiếp tục trong budget1024. MSR không managed trả VMX RDMSR/WRMSR exit31/32.
  Giá trị không hợp lệ trả unsupported trước khi nạp; chưa có #GP injection.
- Host syscall MSRs giữ qua trampoline VMSAVE/VMLOAD. Không dùng địa chỉ MSR user cung cấp
  để thực thi WRMSR trực tiếp. SCE/mode restrictions vẫn giữ, chưa hỗ trợ đầy đủ syscall Linux.
- `msrprobe` kiểm tra guest WRMSR→RDMSR→CPUID, readback userspace, disable→exit32,
  từ chối host HSAVE MSR. Chưa runtime-test trước reboot 0.3.5.
- Build/imports/unit tests PASS; AuxKC xác nhận 0.3.5, install exit0.
  Log `research/runs/0.3.5-install.log`. Hiện loaded0.3.4, cleanup sạch; cần reboot.

### 27. 0.3.5 MSR PASS; 0.3.6 quyền MSR và FS/GS
- Loaded 0.3.5 sau reboot; toàn bộ scripts/vcpu-test.sh --modes PASS gồm lifecycle,
  real/protected32/long64, NPF, TSC và syscall MSR read/write/disable/readback.
  Log `research/runs/0.3.5-vcpu-test.log`.
- Native VZ start qua enable CSTAR/SFMASK, nhưng thread[10] set_msr_access trả unsupported;
  sau đó enable managed ID148 (FS_BASE) unsupported. Cleanup sạch, log `research/runs/0.3.5-vz-start.log`.
- 0.3.6 xử lý trap10: low32 framework MSR ID, high32 quyền READ=1/WRITE=2.
  Intercept phần cứng vẫn bật; guest operation chỉ được emulation khi managed + đúng quyền,
  còn lại trả VMX RDMSR/WRMSR exit31/32. Reject flags lạ và direct access MSR không managed.
- FS_BASE/GS_BASE (framework148/149, internal6/7) alias với VMCS segment base và VMCB
  FS/GS.base. Không tạo bản sao độc lập; canonical checks, cho base khác0 trong long64.
- Mở rộng msrprobe: read-only/write-only permissions, alias FS/GS API→VMCS,
  guest WRMSR/RDMSR và giá trị saved trở lại userspace. Unit tests alias/bounds PASS.
- 0.3.6 build không warning, 339 imports OK, ASan/UBSan memory/modes/vcpu PASS.
  AuxKC xác nhận 0.3.6, install exit0; log `research/runs/0.3.6-install.log`.
  Loaded vẫn 0.3.5, takeover=0/vms=0/hv_support=0; cần reboot trước runtime-test.

### 28. 0.3.6 PASS; 0.3.7 bổ sung SYSENTER (2026-11-07, giờ Việt Nam)
- Loaded 0.3.6; vcpu-test --modes PASS toàn bộ: modes/TSC/syscall MSR, quyền READ/WRITE,
  FS/GS alias và guest write/read/save. Log `research/runs/0.3.6-vcpu-test.log`.
- Native VZ start đã qua CSTAR/SFMASK/FS/GS/KERNEL_GS/LSTAR/STAR và set_msr_access.
  Dừng ở enable managed ID145=IA32_SYSENTER_CS. Log `research/runs/0.3.6-vz-start.log`.
  Không có vcpu_run; task/thread cleanup và restore sạch.
- 0.3.7 thêm cả nhóm SYSENTER_CS/ESP/EIP (framework145/146/147), alias VMCS
  0x482a/0x6824/0x6826 ↔ VMCB0x628/0x630/0x638. Kiểm tra CS32-bit, ESP/EIP canonical.
- Refactor bảng managed MSR chứa address/framework ID/VMCB offset/VMCS alias để tránh
  lặp ánh xạ; accessor ID ngoài phạm vi không truy cập array. VMSAVE/VMLOAD giữ host state.
- msrprobe mở rộng kiểm tra từng SYSENTER MSR: enable/access, API→VMCS, guest WRMSR/RDMSR,
  giá trị trả về userspace và reset. Chỉ kiểm tra MSR state, không tuyên bố SYSENTER transition PASS.
- Build không warning; imports339 và memory/modes/vcpu ASan/UBSan PASS.
  AuxKC đã xác nhận 0.3.7, install exit0; log `research/runs/0.3.7-install.log`.
  Loaded0.3.6, takeover=0/vms=0/hv_support=0; runtime mới còn chờ reboot.

---

## Trạng thái hiện tại

| Hạng mục | Trạng thái |
|---|---|
| Giai đoạn 1 (SVM self-test) | Xong, PASS 24/24 |
| Giai đoạn 2 (takeover trap) | Xong, PASS trên 0.2.2; trace + cleanup đã xác nhận |
| Bản đang nạp trên máy | 0.3.22; vCPU/IRQ/IRETQ và VM/ROM PASS hardware |
| Giai đoạn 3 | EFI đã qua IRQ40, dừngCR8; chuẩn bị0.3.23 CR8/no-TF opt-in |

## Bước tiếp theo

1. Khảo sát cách Apple VMM dùng VM-entry interruption info, interruptibility state,
   interrupt/NMI-window controls và pending debug exceptions (static + probe trên0.3.19).
2. Gom 0.3.20: IF=1 trong bounded runner, interrupt shadow, event/exception injection,
   interrupt-window exits; thêm probe + unit tests; offline gate rồi mới cài AuxKC/reboot.
3. Sau reboot: vCPU --modes, VM --rom, VZ --start --seconds300. Mục tiêu cuối vẫn là
   Podman applehv và Docker native chạy container; QEMU HVF Linux boot chỉ là mốc trung gian.

## Lệnh hay dùng

```
make                      # build kext + hvprobe
make check                # kiểm tra symbol import
make test-memory          # NPT/memory rollback tests (ASan/UBSan, IOKit giả lập)
make test-vcpu            # register/VMCS cache tests (ASan/UBSan)
sudo scripts/vm-test.sh    # VM/NPT integration test
sudo scripts/vcpu-test.sh  # vCPU + bounded real-mode SVM integration test
make kernsyms             # sinh lại kernsyms.h (sau update macOS hoặc rebuild Boot KC)
sudo scripts/install.sh   # cài vào /Library/Extensions + AuxKC, cần reboot
sudo scripts/selftest.sh  # chạy self-test SVM
sudo scripts/trap-trace.sh# takeover → hvprobe → log → gỡ trap AMDHV
scripts/status.sh         # xem trạng thái (không cần root)
sudo scripts/uninstall.sh # gỡ AMDHV, cần reboot
```

Tắt khẩn cấp: thêm boot-arg `-amdhvoff` (AMDHV không start).

## Bài học

- Trên Tahoe, mỗi lần sửa kext = cài + reboot; kext trong AuxKC tự nạp mỗi lần boot, nên mọi thứ
  nguy hiểm phải chạy theo yêu cầu (sysctl), không chạy lúc boot.
- Không để script nuốt lỗi của `kmutil`; luôn kiểm tra lại nội dung AuxKC trước khi reboot.
- Bộ kiểm tra symbol phải hiểu symbol "indirect" của KPI (vd. `IOMalloc` → `IOMalloc_external`).
- Không giả định code và dữ liệu của kernel dời cùng một khoảng trong kernel collection.

## Điều chỉnh quy trình theo phản hồi về reboot (2026-11-07)

Người dùng yêu cầu hạn chế reboot. Đã xác nhận loaded0.3.6, staged0.3.7 và guard
runtimePinned chặn terminate sau takeover. PLAN bổ sung quy trình gom API theo nhóm,
khảo sát ABI và test userspace trước khi cài; tận dụng mỗi boot cho nhiều probe.
Không tiếp tục yêu cầu reboot sau từng MSR riêng lẻ. Chưa thay đổi kext ở lượt này.


## Đợt 0.3.8: giảm reboot bằng kiểm thử theo nhóm

- Thay thế bản staged 0.3.7 chưa boot. Loaded vẫn 0.3.6.
- Khảo sát x86_64 Virtualization XPC: 11 native MSRs và run_until(FOREVER).
  Inventory: `research/native-startup-25G332.json`; script `tools/audit_native_startup.py`.
  Đây là call sites tĩnh, không chứng minh runtime hay bao phủ indirect calls.
- Probe cả 11 MSRs trên 0.3.6 xác nhận 7 có sẵn, thiếu ba SYSENTER và TSC;
  enable/access kiểm tra riêng, cleanup PASS: `research/runs/0.3.6-native-init-group.log`.
- 0.3.8 gồm SYSENTER của 0.3.7 + TSC managed emulation/relative clock cache,
  run_until FOREVER dùng bounded runner; finite deadline vẫn unsupported.
  Không ghi MSR TSC của host. Không quảng cáo preemption timer chưa có.
- Tách handler MSR thực dùng trong kernel thành pure helper để test userspace:
  đủ 11 MSRs, permissions, invalid values, aliases, TSC wrap, host isolation.
- Thêm nativeinitprobe vào hardware gate; tscprobe kiểm tra API TSC read/write,
  guest WRMSR/RDMSR, permission exits và run_until. Chưa chạy các nhánh mới trên máy thật.
- Native entry chưa hỗ trợ được log đầy đủ controls, CR shadows, segments, flags;
  pending FP write bị từ chối rõ ràng vì FPU chưa triển khai, không silently discard.
- `scripts/check-offline.sh` PASS: build, 339 imports, memory/vcpu/modes/MSR ASan/UBSan,
  6 resolver tests và cú pháp scripts. Log `research/runs/0.3.8-offline-checks.log`.
- Sau reboot chạy VM --rom, vCPU --modes và VZ --start. Chưa EFI/Linux boot,
  chưa hoàn thành Phase 3 hay backend Podman/Docker.

- Cài đặt hoàn tất: AuxKC xác nhận `com.fixvm.driver.AMDHV 0.3.8` tại
  `/Library/Extensions/AMDHV.kext`; `kmutil load` trả28 (requires reboot), script xác nhận
  staged thành công. Log `research/runs/0.3.8-install.log`.
  Hiện loaded0.3.6, takeover=0/vms=0/hv_support=0. Dừng tại bước reboot theo yêu cầu người dùng.
  Sau khi người dùng báo đã reboot, xác nhận loaded0.3.8 rồi chạy toàn bộ hardware gates nêu trên.


## 0.3.8 runtime PASS; 0.3.9 host cancellation (2026-11-07)

- Sau reboot đã xác nhận loaded0.3.8. VM/ROM và vCPU --modes PASS toàn bộ,
  gồm native startup11 MSRs, SYSENTER state, TSC API/guest WRMSR/RDMSR/permissions,
  run_until FOREVER, finite deadline rejection, modes và cleanup.
  Logs: `research/runs/0.3.8-vm-test.log`, `research/runs/0.3.8-vcpu-test.log`.
- Native VZ start vẫn FAIL, nhưng cả11 enable/access đã thành công. Trap task13
  vcpu_interrupt(mask1) trả unsupported; chưa gọi vcpu_run. Không kết luận đây là
  nguyên nhân duy nhất: interrupt cũng có thể xuất hiện trong teardown.
  Log `research/runs/0.3.8-vz-start.log`. Cleanup takeover0/vms0/hv_support0.
- LLDB attach vào Apple XPC bị macOS từ chối; không sửa bảo vệ hệ thống để attach.
  Static audit giải được80/84 direct VMCS write sites; tất cả encoding giải được
  nằm trong cửa sổ writable1..80. Bốn sites còn lại không đủ immediate evidence.
- 0.3.9 bổ sung task13 bitmap request qua mailbox riêng. Lock order gate→interruptGate;
  interrupt chỉ giữ interruptGate, không chờ global gate đang giữ xuyên guest run.
  Pending bits atomic, runner kiểm tra trước entry và giữa mỗi bước bounded VMRUN.
  Live mask được kiểm tra toàn bộ trước mutation; destroy/reuse xóa pending.
- Host cancellation trả VMX external-interrupt exit1, instruction length0, không tăng RIP;
  không inject guest IRQ, không quảng cáo APIC hay unbounded/IPI-based execution.
  Các request trùng nhau coalesce. NPT/registry lifetime vẫn được global gate bảo vệ.
- Thêm interruptprobe: empty/invalid batch, pre-entry cancellation, single consumption,
  cross-thread kick và slot reuse. Cross-thread probe chấp nhận pre-entry hoặc in-flight,
  không coi nó là chứng minh đã ngắt đúng giữa một lệnh guest.
- Unit helper thực kiểm tra bitmap/coalescing/lifetime và10.000 producer/consumer handshakes;
  ASan/UBSan và một lần ThreadSanitizer đều PASS. Command TSAN:
  `clang++ -std=c++17 -Wall -Wextra -fsanitize=thread -g -IAMDHV/src tools/tests/vcpu_interrupt_test.cpp -o build/vcpu_interrupt_tsan`
  rồi `build/vcpu_interrupt_tsan`.
- Bổ sung snapshot CPU khi bị hủy trước first run để chẩn đoán lỗi native startup sớm.
  Không sửa dữ liệu Podman. Native EFI/Linux boot và mục tiêu container vẫn chưa đạt.

- Cài0.3.9 hoàn tất: `kmutil load` trả28 yêu cầu reboot; AuxKC xác nhận
  `com.fixvm.driver.AMDHV 0.3.9 /Library/Extensions/AMDHV.kext`, script exit0.
  Log `research/runs/0.3.9-install.log`. Build không warnings; toàn bộ offline gate PASS
  trong `research/runs/0.3.9-offline-checks.log`.
  Máy vẫn loaded0.3.8, takeover0/vms0/hv_support0. Dừng chờ reboot theo yêu cầu.


## 0.3.9 runtime PASS; 0.3.10 native reset profile (2026-11-07)

- Loaded0.3.9, toàn bộ vCPU --modes PASS, gồm host cancellation: bitmap invalid batch,
  pre-entry exit1, giữ RIP, consume một lần, cross-thread requests và destroy/reuse.
  Log `research/runs/0.3.9-vcpu-test.log`.
- Native VZ đã tới run_until: lần đầu consume pending interrupt thành công; lần tiếp
  reject mode. Trace ghi CR0=30, CR4=2000, masks=e0000020/2000, shadows=60000010/0,
  primary controls=200080, CS=f000/baseffff0000, RIP=fff0. Log0.3.9-vz-start.log.
  Start completion có error=nil nhưng VM đã ở Error state3; probe vẫn FAIL đúng,
  không coi completion thành công là guest đã chạy. Cleanup sạch.
- 0.3.10 mở rộng real-mode profile cho NE và VMX host-owned VMXE. AMD CR4 không nạp
  VMXE; MOV CR0/CR4 được emulation từ guest state + mask/shadow, không lộ forced TS/MP.
- Primary HLT-exiting được chấp nhận. TPR-shadow chỉ được chấp nhận trong real-mode
  profile không guest IRQ/virtual APIC, TPR threshold0: CR8 không thực thi ở mode này.
  Protected/long-mode TPR-shadow vẫn reject; không quảng cáo virtual APIC capability.
- MOV CR decode dùng SVM decode-assist valid bit63, register bits0..3 và NRIP1..15.
  Read/unchanged write được consume; thay đổi masked bit trả VMX exit28 đúng qualification,
  không tăng RIP. Unmasked state/mode transition chưa có validator đầy đủ nên reject.
  CLTS/LMSW, CR3/CR8 và general mode transitions chưa triển khai.
- Pure helper test cho reset/masks/shadows/decode/invalid inputs PASS ASan/UBSan.
  resetprobe tái hiện high ROM reset vector và kiểm tra CR reads, masked write exit,
  unchanged write→HLT, unsupported mode transition. Hardware mới còn chờ reboot.
- Bổ sung exit diagnostics info1/info2/NRIP/CSbase/CRs; vzprobe delegate ghi stop error.
  Native EFI/Linux boot chưa đạt, mục tiêu Podman/Docker chưa hoàn tất.

- VM/ROM regression trên0.3.9 cũng PASS: `research/runs/0.3.9-vm-test.log`.
- Offline0.3.10 build/imports và tất cả unit tests PASS, không warnings:
  `research/runs/0.3.10-offline-checks.log`.
- Đã cài0.3.10: AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0, log `research/runs/0.3.10-install.log`. Máy vẫn loaded0.3.9,
  takeover0/vms0/hv_support0. Dừng chờ reboot theo yêu cầu người dùng.


## 0.3.10 hardware PASS; 0.3.11 descriptor tables + PE transition (2026-11-07)

- Loaded0.3.10: toàn bộ CPU regression PASS gồm reset CR0/CR4, high ROM execution,
  masked MOV-CR exit, unchanged write, unsupported EM change và cleanup.
  Log `research/runs/0.3.10-vcpu-test.log`. VM/ROM regression cũng PASS:
  `research/runs/0.3.10-vm-test.log`.
- Native EFI đã thực thi qua reset vector, tới RIPff4c/CSbaseffff0000,
  exit6b=GDTR_WRITE (LGDT), NRIPff51. Không còn reject reset entry.
  Log `research/runs/0.3.10-vz-start.log`. Chưa boot xong EFI/Linux.
- 0.3.11 cho SGDT/SIDT/LGDT/LIDT thực thi trong guest với NPT và single-step budget.
  GDTR/IDTR được nạp từ state và save/publish sau exit, thay vì hardcode base0/limit.
  Validation: limit16-bit, legacy base32-bit hoặc canonical base trong long64.
  VMRUN/VMEXIT tự save/restore host GDTR/IDTR; LLDT/LTR vẫn intercept.
- Bổ sung CR0.PE 0→1 khi chỉ thay đổi PE, paging/EFER không đổi. CR0 masked changes
  vẫn về VMX exit28; các thay đổi khác tiếp tục reject. Hardware CR0 vẫn forced TS/MP.
- Protected legacy validation cho cached16-bit segments và ring0 GDT16/32-bit segments;
  hỗ trợ far jump/MOV segment rồi resume. TPR-shadow chỉ legacy, CR8 vẫn intercept/
  unsupported, không guest IRQ/APIC. Paging transitions, PE1→0, FPU/IRQ chưa triển khai.
- descriptorprobe mới: LGDT/LIDT/SGDT/SIDT→CPUID, table readback, resume→setPE→far jump→
  protected32 RAM marker→CPUID→resumeHLT; faulting LGDT operand qua NPT phải không đổi GDTR.
  Test mới chưa chạy trên hardware trước reboot.
- Build/imports và toàn bộ ASan/UBSan/unit/resolver/script checks PASS không warning:
  `research/runs/0.3.11-offline-checks.log`. Các negative tests được cập nhật sang
  unsupported paging/EM vì PE transition giờ là chức năng được triển khai.

- Cài0.3.11 hoàn tất: AuxKC xác nhận `com.fixvm.driver.AMDHV 0.3.11` tại
  `/Library/Extensions/AMDHV.kext`; kmutil load exit28 yêu cầu reboot, script exit0.
  Log `research/runs/0.3.11-install.log`. Loaded vẫn0.3.10; lần kiểm tra cleanup
  takeover0/vms0/hv_support0. Dừng chờ reboot theo yêu cầu người dùng.


## 0.3.11 hardware PASS; 0.3.12 legacy controls và bounded yield (2026-11-09)

- Loaded0.3.11; CPU regression PASS toàn bộ, gồm descriptorprobe: GDT/IDT load/store,
  table persistence, PE→far jump→protected32 marker, resumeHLT và unmapped LGDT operand.
  Log `research/runs/0.3.11-vcpu-test.log`. VM/ROM regression PASS:
  `research/runs/0.3.11-vm-test.log`.
- Native EFI qua LGDT, MOV CR0 tạiff57 trả VMX28 thành công. Apple VMM sửa state rồi
  resumeff5a với CR0=23, CR4=2000, shadowCR0=40000023, GDTR=ffffff80/27; validator
  cũ reject. Log `research/runs/0.3.11-vz-start.log`. Cleanup sạch.
- Khảo sát ROM thật `VZG11.fd` bằng Capstone trong venv tạm, không sửa framework.
  Script `tools/audit_efi_reset.py`, output có SHA256 `research/efi-reset-25G332.txt`.
  Chuỗi tĩnh: far jump→CR4=640→ROM scan→CPUID→page table fill→CR3=800000→CR4.PAE→EFER/PG.
  Static disassembly không được tính là boot PASS.
- 0.3.12 hỗ trợ legacy CR0 PE/MP/ET/NE (VMCS ET có thể0 như Apple), hardware và guest
  CR0 reads giữ ET cố định1, forced TS/MP không lộ vào guest cache. Mask/shadow vẫn áp dụng.
- CR4 MCE/OSFXSR/OSXMMEXCPT/PAE được chuẩn bị khi paging tắt; VMXE chỉ private/host-owned.
  FPU vẫn bị chặn bằng TS/MP và exception intercept; không tuyên bố FPU context switch.
- CR3 read/write/staging hỗ trợ địa chỉ page-aligned tối đa48bit khi paging/EFER tắt.
  Merge masked CR0/CR4 sau validation; unsupported paging/OSXSAVE/EM/TS/PE1→0 reject.
- Hết1024 bước nội bộ trả VMX host exit1 với length0 thay vì unsupported, cho VMM resume
  vòng lặp dài. Mỗi trap vẫn có giới hạn, không tắt budget hay chạy guest vô hạn.
- controlprobe mới kiểm tra actual CR0=23, CR4=640/660, CR3=800000, masks/ET,
  vòng lặp5000 iterations qua nhiều yield và FPU vẫn bị chặn. Chưa hardware-test0.3.12.
- Offline build/imports/ASan/UBSan/resolver/script tests PASS, không warning:
  `research/runs/0.3.12-offline-checks.log`. EFI/Linux boot vẫn chưa đạt.

- Đã cài0.3.12: AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.12-install.log`. Loaded vẫn0.3.11,
  takeover0/vms0/hv_support0 trước cài. Dừng tại bước reboot theo yêu cầu người dùng.


## 0.3.12 hardware PASS; 0.3.13 long-mode entry (2026-11-09)

- Loaded0.3.12; CPU regression PASS toàn bộ, gồm controlprobe CR0/CR4/CR3, vòng lặp
  5000iterations qua nhiều bounded host exits và xác nhận FPU vẫn bị chặn.
  Log `research/runs/0.3.12-vcpu-test.log`.
- Native EFI đi qua ROM scan/page-table fill, EFER RDMSR/WRMSR trả VMX31/32;
  Apple VMM xử lý rồi resume tạifffffe5f với CR0=23, CR3=800000, CR4=2660, EFER=100,
  CSAR=c09b. Validator cũ reject trạng thái LME-preparation.
  Log `research/runs/0.3.12-vz-start.log`; cleanup sạch. Chưa Linux boot.
- 0.3.13 phân biệt LME-preparation (PG0/PE1/PAE1/EFER100), IA-32e compatibility mode
  (PG1/PE1/PAE1/EFER500/CS.L0) và 64-bit code sau far jump (CS.L1).
- Chấp nhận native CR0/CR4 register layout, selectors ring0 GDT không hardcode8/16,
  và VM-entry IA32e bit9 khi EFER500. Vẫn reject PG thiếu LMA/PAE, CS.L+DB cùng bật,
  privilege bits lạ, pointer/CR3 không hợp lệ, entry controls chưa hỗ trợ.
- Primary TPR-shadow bit21 chỉ trong bounded pilot: CR8 vẫn intercept/unsupported,
  threshold phải0, không APIC virtualization hay guest IRQ. Đây không phải triển khai
  TPR-shadow đầy đủ. FPU, interrupt injection, paging transitions tổng quát vẫn thiếu.
- longtransitionprobe: LME preparation→masked PG MOV-CR exit→userspace publish PG/LMA→
  compatibility entry→far jump→64-bit RAM marker→CPUID→resumeHLT; tháo leaf PT phải NPF.
  Đây là test mới, chưa hardware-test0.3.13. Masked PG vẫn do userspace VMM xử lý;
  không tuyên bố driver đã tự emulation mọi paging transition.
- Offline build/imports/ASan/UBSan/resolver/script checks PASS, không warning:
  `research/runs/0.3.13-offline-checks.log`.

- VM/ROM regression0.3.12 PASS: `research/runs/0.3.12-vm-test.log`.
- Cài0.3.13 hoàn tất, AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.13-install.log`. Loaded vẫn0.3.12,
  takeover0/vms0/hv_support0 trước cài. Dừng chờ reboot theo yêu cầu người dùng.


## 0.3.13 hardware PASS; 0.3.14 I/O exit group (2026-11-09)

- Loaded0.3.13; toàn bộ CPU regression PASS gồm longtransitionprobe: LME preparation,
  PG exit→VMM state update→compatibility entry→far jump64→RAM marker→resumeHLT,
  NPT page-walk fault. Log `research/runs/0.3.13-vcpu-test.log`.
- Native EFI thật đã qua far jump64, tới RIPfffe150e/CSbase0, CR0=8000003b,
  CR4=660, exit7b (IOIO), info1=3fb0210, nextRIPfffe150f: OUT byte quaDX tớiport3fb.
  Log `research/runs/0.3.13-vz-start.log`. Chưa EFI/Linux boot hoàn chỉnh.
- 0.3.14 chuyển IOIO sang VMX exit30 với port/width/direction/string/REP/immediate,
  instruction length và instruction-info cho string. Giữ RIP/GPR/count/memory,
  userspace VMM chịu trách nhiệm mô phỏng thiết bị và advance. Không execute host IN/OUT.
- Để phân biệt immediate/DX đúng kể cả prefixes: đọc đúng1..15bytes qua backing đã pin,
  sau khi host state/interrupts đã restore và vẫn giữ backend gate. Không dereference GPA.
  Reader kiểm tra mapping live/NPT readable/executable; walker hỗ trợ unpaged/IA32e
  4KiB/2MiB/1GiB, canonical addresses, bounds, present/NX/large-page alignment.
- Opcode/prefix phải khớp metadata SVM (width,port,direction,address-size,segment/REP).
  Trường hợp decode không hợp lệ hoặc không đọc được fail closed, không đoán encoding.
  IO bitmap vẫn chặn toàn bộ port.
- Unit test thực cho IO translator/fetch, thêm memory permission/hole tests PASS ASan/UBSan.
  ioprobe mới gồm12 scalar cases mỗi mode16/32/64, userspace-resume, REP IN/OUT,
  giữ nguyên registers/data và instruction qua page boundary. Chưa hardware-test0.3.14.
- Build/imports339 và toàn bộ offline gate PASS, không warning:
  `research/runs/0.3.14-offline-checks.log`. FPU/IRQ/APIC/general execution còn thiếu.

- VM/ROM regression0.3.13 PASS: `research/runs/0.3.13-vm-test.log`.
- Cài0.3.14 hoàn tất, AuxKC xác nhận version/path đúng; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.14-install.log`. Loaded vẫn0.3.13,
  takeover0/vms0/hv_support0 trước cài. Dừng chờ reboot theo yêu cầu người dùng.

- Người dùng cho phép assistant thực hiện reboot để kích hoạt0.3.14. Trước reboot đã
  xác nhận AuxKC0.3.14, takeover0/vms0/hv_support0. Sau khi kết nối lại: xác nhận
  loaded0.3.14 rồi chạy VM --rom, vCPU --modes (ioprobe), VZ --start và lưu logs.

## 0.3.14 hardware PASS; 0.3.15 x87/MMX/SSE batch (2026-11-09)

- Sau reboot xác nhận loaded0.3.14. Toàn bộ vCPU --modes PASS, gồm ioprobe scalar
  16/32/64, REP metadata, userspace resume, instruction qua page boundary.
  VM --rom PASS. Logs `research/runs/0.3.14-vcpu-test.log`, `0.3.14-vm-test.log`.
- VZ --start lần đầu PASS lifecycle nhưng dừng ngay khi completion trả Running;
  trace chưa vượt vòng dựng page table. Đây không phải bằng chứng EFI boot.
  Sửa vzprobe giữ VM chạy quan sát15giây và ghi nhận lỗi delegate.
- Lần chạy quan sát FAIL rõ: native EFI qua IOIO→VMX30 nhiều lần rồi #NM(exit47)
  tại RIPfffe1353. Disassemble ROM xác nhận WAIT; FNINIT; FLDCW; MOV CR4; LDMXCSR.
  Log `research/runs/0.3.14-vz-observe.log`; teardown takeover0/vms0/hv_support0.
- 0.3.15 thêm private512byte FP image/reset, pull flag80 từ RW FP và publish RO/RW,
  giữ partial-write baseline. MXCSR reserved/DAZ input bị reject trước FXRSTOR;
  lỗi nhớ qua sync cho tới explicit valid rewrite. Không bỏ pending FP writes.
- Assembly VMRUN đổi x87/MMX/SSE trong cửa sổ GIF0: save host XSAVE mask3,
  load guest FXSAVE64 image, chạy, save guest, restore host XSAVE mask3 và CR0 chính xác.
  Buffer riêng đã pin/page-aligned. Host TS tạm clear; FFXSR tạm tắt để không bỏ XMM.
  Host phải OSXSAVE/OSFXSR/XCR0[1:0]=3 và EM0; không đổi XCR0 của host.
- Guest CR4.OSXSAVE vẫn reject, XSETBV intercept: AVX/AVX512 guest chưa hỗ trợ.
  Guest CR0 hardware force ET/NE để x87 fault về #MF bị intercept, không phát IRQ13;
  không tuyên bố hỗ trợ ngoại lệ FPU/legacy IRQ injection. Không còn force TS/MP.
- Dùng cùng FP-switch assembly macros trong userspace test: 1000lượt host có dữ liệu
  và1000lượt host init-state, kiểm x87/XMM/YMM-upper/XSTATE_BV PASS ASan/UBSan.
  Đây chưa phải test VMRUN của0.3.15.
- fpprobe cho lần boot sau:16/32/64bit WAIT/FNINIT/x87 arithmetic/SSE, raw vCPU trap
  giữ host FP/YMM, publish/resume, MMX memory+EMMS, unsupported exception cleanup,
  invalid MXCSR/rewrite recovery, reject OSXSAVE. Controlprobe cập nhật FNINIT→HLT.
- Offline gate PASS (build/imports339, ASan/UBSan, resolver6cases, shell syntax).
  Log `research/runs/0.3.15-offline-checks.log`. Hardware0.3.15 pending; chưa Linux boot.

- Cài0.3.15 hoàn tất: AuxKC xác nhận version/path đúng, kmutil load exit28 yêu cầu
  reboot, install script exit0. Log `research/runs/0.3.15-install.log`.
  Loaded vẫn0.3.14; takeover0/vms0/hv_support0. Dừng tại ranh giới reboot;
  sau reboot xác nhận loaded0.3.15 rồi vCPU --modes, VM --rom, VZ --start.

## 0.3.15 hardware PASS; 0.3.16 active-paging controls (2026-11-09)

- Loaded0.3.15; toàn bộ vCPU --modes PASS. fpprobe16/32/64 xác nhận x87 WAIT/FNINIT/
  arithmetic, SSE, MMX/EMMS, FP ABI, host x87/XMM/YMM+XSTATE_BV preservation trên
  raw trap, invalid MXCSR rejection/recovery và unsupported-exception restoration.
  Log `research/runs/0.3.15-vcpu-test.log`. VM/ROM PASS: `0.3.15-vm-test.log`.
- Native VZ15giây observation FAIL sau khi qua FPU, chạy code trong RAM1fffxxxx.
  Điểm dừng mới: SVMexit10 (MOV CR0), RIP1fff0d2c, NRIP1fff0d2f, operand sourceRAX.
  Hardware CR0=80000033/CR4=660. Log `research/runs/0.3.15-vz-start.log`.
  Teardown sạch takeover0/vms0/hv_support0. Chưa EFI/Linux boot hoàn chỉnh.
- Extract firmware vào/tmp bằng uefi-firmware-parser; DxeIpl PE có MOV RAX,CR0;
  OR RAX,10000; MOV CR0,RAX tạiRVA1d23..1d2c. Inferred loadbase1ffef000 khớp
  stopRIP; đây là static inference, không phải dump runtime memory. Script
  `tools/audit_efi_paging.py`, hashes/evidence `research/efi-paging-25G332.txt`.
- 0.3.16 gom CR0.WP/AM, CR4.PSE/PGE, CR3 root replacement khi đã ở long mode;
  mask/shadow vẫn trảVMX28 trước commit. TLB flush-all mỗi entry bao gồm global pages.
  Giữ reject PG transitions trực tiếp, PE1→0, PAE clear trongIA32e, EM/TS/CD/NW,
  OSXSAVE/PCIDE/LA57, CR3 low flags/nonaligned/no-flush/high addresses.
- Thêm rejected-CR operand diagnostic cho các trường hợp chưa hỗ trợ tiếp theo.
  Unit tests xác nhận valid updates, masks, no-state-mutation-on-reject và mode gating.
- pagingcontrolprobe mới: guest đổi WP/AM/PSE/PGE/CR3, hai root huge-page global
  ánh xạ cùng code sang hai vùng data khác nhau, switch-back, WP0 cho supervisor
  write qua guest RO page, WP1 phải fault trước write. Chưa hardware-test0.3.16.
  Guest page-fault injection vẫn chưa triển khai; test mong HV_UNSUPPORTED.
- Offline gate PASS, không warning; imports339, ASan/UBSan/FPU-preservation,
  resolver6cases/shell syntax. Log `research/runs/0.3.16-offline-checks.log`.

- Cài0.3.16 hoàn tất: AuxKC đúng version/path, kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.16-install.log`. Loaded vẫn0.3.15;
  takeover0/vms0/hv_support0. Dừng ở bước reboot để kích hoạt bản mới.

## 0.3.16 hardware PASS; 0.3.17 NXE paging (2026-11-09)

- Loaded0.3.16. Toàn bộ vCPU --modes PASS, gồm pagingcontrolprobe: WP/AM/PSE/PGE,
  CR3 đổi root thật, flush global translations, supervisor write với WP0 và WP1
  fault trước write. Logs `research/runs/0.3.16-vcpu-test.log`, VM/ROM PASS
  `research/runs/0.3.16-vm-test.log`.
- Native EFI vượt MOV CR0 tại1fff0d2c. Sau RDMSR/WRMSR tại1fff0d98/1fff0d9f,
  Apple VMM publish EFER=d00 (NXE+LMA+LME), RIP1fff0da1, CR0=80010033,
  CR3=800000, CR4=2660. Validator cũ chỉ nhận EFER500 nên reject trước entry.
  Log `research/runs/0.3.16-vz-start.log`; cleanup takeover0/vms0/hv_support0.
- 0.3.17 coi NXE độc lập với legacy/LME/LMA, áp dụng nhất quán vào mode validator,
  VM-entry controls, CR emulation và instruction fetch. EFER thực d00 vẫn được
  đưa vào VMCB; không xóa NXE của guest. SCE/SVME/reserved bits vẫn bị reject.
- Guest code reader tiếp tục chặn NX ở mọi cấp, vì NXE1 cấm fetch còn NXE0 coi NX
  là reserved. Unit coverage thêm NXE-on 4K/2M/1G và parent-level NX.
- nxprobe mới: EFER WRMSR→VMM bậtNXE→resume, IO decode NXE-on, đọc/ghi dataNX,
  chặn fetch tại từng cấp paging, huge2M/1G, NXE-off reserved NX, clearNX/recovery.
  pagingcontrolprobe chạy cả EFER500 vàd00. Chưa hardware-test0.3.17.
- Offline gate PASS không warning, imports339, ASan/UBSan/FPU-preservation,
  resolver6cases và shell syntax: `research/runs/0.3.17-offline-checks.log`.
  IRQ/APIC, guest exception injection, AVX, SMP và performance còn thiếu; chưa Linux boot.

- Cài0.3.17 hoàn tất: AuxKC đúng version/path, kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.17-install.log`. Loaded0.3.16 chưa thay;
  takeover0/vms0/hv_support0. Dừng tại reboot, sau đó chạy các hardware gate nêu trên.

## 0.3.17 hardware PASS, long observation; 0.3.18 flags/TLB batch (2026-11-09)

- Loaded0.3.17; vCPU --modes PASS toàn bộ, gồm NXE WRMSR→VMM update, NX data,
  parent/leaf NX,2MiB/1GiB NX, NXE-off reserved faults và paging controls500/d00.
  VM/ROM PASS. Logs `research/runs/0.3.17-vcpu-test.log`, `0.3.17-vm-test.log`.
- VZ15giây PASS lifecycle, chưa chứng minh boot. Thêm userspace option
  `scripts/vz-probe.sh --start --seconds 1..300`; reject invalid args trước takeover.
  Quan sát120giây PASS start/hold/stop/cleanup (`0.3.17-vz-observe120.log`).
- Đối chiếu353 RIP offsets trong page1ffba000 với instruction boundaries của các
  PE đã extract: DxeCore RVApage15000 khớp353/353; đây là static inference (base1ffa5000),
  không phải runtime dump. Mã có cấu trúc range decoder/giải nén. Venv nghiên cứu
  `.venv-research` chứa capstone/uefi_firmware; không là dependency build kext.
- Bài300giây tìm được lỗi sau đoạn giải nén: IO exits tại1fb400fe/1fb40105,
  NPF→VMX48 tại1fb4014b, rồi PUSHF(exit70) tại1fb401ad, NRIP1fb401ae.
  Log `research/runs/0.3.17-vz-observe300.log`; không Linux boot. Cleanup sạch.
- 0.3.18 xử lý PUSHF/POPF bằng CPU thật: tạm bỏ TF, dùng DR0 execution breakpoint
  tại NRIP để giới hạn đúng một lệnh. Stack translation/permissions/A-D bits do
  CPU xử lý; không ghi stack bằng host memory emulation. Xác nhận B0/nextRIP hoặc
  known pre-execution exit ởnextRIP; restore internalTF sau đó. DR0 host được
  restore trước STGI; có kiểm tra/restore phòng vệ cuối quantum.
- Hỗ trợ guest DF; POPF đặt IF/TF/IOPL/flags ngoài subset trảunsupported sau lệnh,
  trước khi chạy lệnh kế tiếp. Không tuyên bố guest IRQ/debug virtualization.
  Reject NRIP invalid/segment overflow/noncanonical và 32bit linear-address wrap.
- Giảm TLB flush từ mỗi bước xuống đầu quantum và sau handled CR writes/INVLPG.
  Gate giữ nguyên, IRQs off, không đổi CPU/NPT/ASID trong quantum. Mỗi lần run mới
  luôn flush-all; same-value MOV CR3 cũng flush. INVLPG dùng full flush, không chạy
  host INVLPGA; guest INVLPGA vẫn unsupported. Budget1024 và cancel không đổi.
- Thêm flagsprobe16/32/64 (prefix, adjacent CPUID/NOP, DF/CF, cross-page stack,
  NPT write fault, unsupported POPF IF), tlbprobe (guest PTE rewrite+INVLPG/CR3,
  NPT remap/protect giữa calls), budgetprobe. Chưa hardware-test0.3.18.
- Baseline0.3.17:100000iterations/293calls/0.177861sec, log `0.3.17-budget-test.log`.
  Chỉ một mẫu bounded-loop, không phải production benchmark hay số đo tăng tốc.
- Offline gate PASS, không warning, imports339, ASan/UBSan/FPU tests/resolver/shell.
  `research/runs/0.3.18-offline-checks.log`. Đã review compiled transition offsets
  GPR40,guestPA48,hostPA56,guestFP32,hostFP24 và restoreDR0/CR0 trước STGI.

- Cài0.3.18 hoàn tất: AuxKC đúng version/path; kmutil load exit28 yêu cầu reboot,
  script exit0. Log `research/runs/0.3.18-install.log`. Loaded vẫn0.3.17;
  takeover0/vms0/hv_support0. Dừng tại reboot để kích hoạt hardware gate mới.

## 0.3.18 hardware verification (2026-11-10)

- Sau reboot xác nhận loaded0.3.18; idle takeover0/vms0/hv_support0.
- vCPU --modes PASS toàn bộ, gồm PUSHF/POPF16/32/64, prefix, DF, cross-page
  stack, NPT readonly fault, POPF IF rejection trước lệnh kế tiếp; INVLPG và
  same-value CR3 thấy PTE global mới trong cùng quantum; NPT remap/protect giữa calls.
  Log `research/runs/0.3.18-vcpu-test.log`. VM/ROM PASS: `0.3.18-vm-test.log`.
- Bounded-loop100000iterations/293calls/0.158093sec so với một mẫu0.3.17
  0.177861sec. Không đủ mẫu để kết luận tăng tốc hay production performance.
- VZ observation300giây FAIL tại PAUSE SVMexit77, RIP1fb401d8, NRIP1fb401da.
  Đã vượt PUSHF và thêm NPF/I/O tại1fb401b1/1fb401bd/1fb401cc. Cleanup sạch
  takeover0/vms0/hv_support0. Log `research/runs/0.3.18-vz-observe300.log`.
  Chưa có bằng chứng Linux boot.

## 0.3.19 PAUSE bounded execution (2026-11-10)

- Static module GUID c8339973-a563-4561-b858-d8476f9defc4 có PUSHF tạiRVA11ad,
  PAUSE tạiRVA11d8, khớp runtimeRIP với inferredbase1fb3f000. PAUSE nằm trong
  vòng chờ port input, sau đó restore IF trước đó bằng CLI/STI. Evidence/hash/
  disassembly `research/efi-pause-25G332.txt`; đây là inference, không runtime dump.
- Giữ PAUSE intercept; consume spin hint bằng validated NRIP, không thay flags/GPRs,
  không chạy host PAUSE hay bỏ budget. Mỗi hint tính một trong1024bước; spinloop
  vẫn phải yield/cancel. Không bật VMX PAUSE-exiting control chưa hỗ trợ.
- Thêm PAUSE vào known pre-execution boundary sau PUSHF/POPF để không reject
  khi instruction intercept thắng execution breakpoint tại cùng nextRIP.
- flagsprobe thêm16/32/64:1536PAUSE qua quantum, 1024hint giữ nguyênflags/registers/
  stack, infinitePAUSE/JMP yield, PUSHF/POPF ngay trước PAUSE. Hardware pending.
- Offline PASS, không compiler warning, imports339, ASan/UBSan, FP-preservation,
  resolver6cases/shell syntax: `research/runs/0.3.19-offline-checks.log`.
- AMD APM Vol2 PAUSE instruction intercept reference:
  https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content
- Sau reboot: xác nhậnloaded0.3.19, chạy vCPU --modes, VM --rom, VZ --start
  --seconds300; không coi lifecycle PASS là EFI/Linux boot. IRQ/APIC/SMP,
  exception injection và production performance vẫn còn thiếu.

- Cài0.3.19 hoàn tất: AuxKC đúngversion/path, kmutil load exit28 yêu cầu reboot,
  install script exit0. Log `research/runs/0.3.19-install.log`. Loaded vẫn0.3.18;
  takeover0/vms0/hv_support0. Dừng tại ranh giới reboot để kích hoạt bản mới.

## 0.3.19 hardware verification (2026-11-10)

- Sau reboot xác nhận loaded0.3.19; idle takeover0/vms0/hv_support0.
- vCPU --modes PASS toàn bộ, 0 FAIL, gồm PAUSE16/32/64: 1024hint giữ flags/registers/
  stack, PAUSE qua quantum, vòng PAUSE/JMP vô hạn yield, PUSHF/POPF→PAUSE boundary.
  Bounded-loop100000iterations/293calls/0.150805sec (một mẫu, không phải benchmark).
  Log `research/runs/0.3.19-vcpu-test.log`. VM --rom PASS32/0: `0.3.19-vm-test.log`.
- VZ --start --seconds300 FAIL: start Running rồi "VM stopped unexpectedly" (VZError1).
  Native EFI đã vượt PAUSE, đi qua nhiều IO/NPF/CPUID/RDMSR/WRMSR và chạy trong DxeCore
  (GUID d6a2cb7f…, inferred base1ffa5000). Ring log64KB bị ghi đè, chỉ còn phần cuối.
- Blocker: entry validator reject với RFLAGS=206 (**IF=1**), RIP1ffb1d9d, CR0=80010033,
  CR3=1fc01000, CR4=2660, EFER=d00, primary=200080, injection=0. PilotFlags=0xcd7 không
  gồm IF. Lệnh tại RVAcd9d là `add eax,eax` (vòng tìm bit), không phải STI: IF được bật
  trong quantum trước. SVM không có STI intercept nên STI chạy native trong single-step;
  DxeCore có STI tại RVA18ca8/1907d (static, không phải runtime dump). Suy luận: CPU arch
  protocol đã cài và CoreRestoreTpl bật ngắt. Cleanup sạch takeover0/vms0/hv_support0.
  Log `research/runs/0.3.19-vz-observe300.log`. Chưa Linux boot.
- Nhóm việc tiếp theo là guest interrupts: chạy guest với IF=1 (STI/MOV SS interrupt
  shadow ↔ VMCS interruptibility0x4824), VM-entry event injection (0x4016/0x4018/0x401a →
  VMCB EVENTINJ), interrupt-window exiting (primary bit2 → VINTR intercept), NMI-window,
  exception injection (#PF/#GP/#UD) và HLT khi IF=1. Đây là đợt lớn, cần khảo sát ABI
  injection của Apple VMM (VirtualMachine-25G332.s) trước khi viết code.

## CachyOS: đối chiếu KVM cho guest interrupts (2026-11-10)

- Môi trường: CachyOS, Linux 7.3.3-1-cachyos, dự án tại `/mnt/data/Code/FixVM` (ntfs3, rw,
  nhãn Data, fs UUID 01DCFED298045400; UUID 43C00220… trong handoff là định danh phía macOS).
  Không đụng phân vùng/VM Podman cũ, không cài kext, không dùng sudo.
- CPU/microcode/SVM khớp macOS: 9900X, microcode0x0b404038, SVM rev1, 32768ASID,
  0x8000000A edx=febfbdff; kvm_amd npt/nrips/vgif/vnmi/avic bật. Log
  `research/linux/2026-11-10-cachyos-7.3.3-sysinfo.txt`, tái tạo `research/linux/collect.sh`.
- `research/linux/kvm-ref/kvmref.c`: guest64 CR0=80010033/EFER=d00, không irqchip. 13/13 PASS,
  20 lần trên 20 core. Đo được: IRQ window mở sau lệnh kế STI; SVM shadow 1 bit (KVM báo 3);
  TF: STI có #DB kế tiếp, MOV SS nuốt #DB (2 lệnh/bước); inject #GP(err)/#UD/IRQ/NMI đúng frame;
  NMI masked tới IRETQ; HLT IF=1 exit.
- Static: Apple VMM tự reject trước entry nếu 0x4824 có cả STI+MOVSS, STI khi IF=0, bit≥0x20,
  hoặc ext-int pending khi IF=0/có shadow (VirtualMachine-25G332.s 0x10022C26E…0x10022C499).
- Phân tích và đề xuất phạm vi0.3.20: `research/linux/KVM-INTERRUPT-REFERENCE.md`.
  KVM PASS không chứng minh AMDHV/Apple backend PASS; cần hardware gate trên macOS.

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

## 0.3.20 hardware verification (2026-11-10)

- Loaded0.3.20. vCPU --modes PASS toàn bộ, gồm IF/shadow/window16/32/64,
  MOVSS→PUSHF, POPF IF, HLT IF1 và injection/NMI rejection. VM --rom PASS.
  Logs `research/runs/0.3.20-vcpu-test.log`, `0.3.20-vm-test.log`.
- VZ300s FAIL sau khi vượt IF=1 và chạy tới module1a1e4886…: SVMexit20
  (MOVfromDR0), RIP1fa6320a, NRIP1fa6320d. Cleanup takeover0/vms0/hv_support0.
  Log `research/runs/0.3.20-vz-observe300.log`; chưa Linux boot.

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

## 0.3.21 hardware verification (2026-11-10)

- Loaded0.3.21, vCPU --modes PASS toàn bộ, gồm debugprobe16/32/64: virtualDR
  roundtrip, fixedDR6/7, REX GPR, breakpoint-enable rejection, VMX29qual,
  shadow/window và SLDT/STR. Log `research/runs/0.3.21-vcpu-test.log`.
- VM/ROM PASS: `research/runs/0.3.21-vm-test.log`.
- VZ300s FAIL sau khi vượt debug save. Apple VMM nhậnVMX7 interrupt-window tại
  RIP1fa5b8c1 rồi publish injection80000040 (external vector40), IF1/shadow0,
  CS38/GDTR1fbea618/47, IDTR1f20a018/fff, EFERd00. Validator hiện rejectinjection.
  Log `research/runs/0.3.21-vz-observe300.log`. Teardown sạch0/0/0; chưaLinuxboot.

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

## 0.3.22 hardware verification (2026-11-10)

- Loaded0.3.22; toàn bộ vCPU --modes PASS, gồm IRQinterrupt/trap frame, handlerPUSHF
  khôngTF, IRETQrestore, readonlycross-pagepreflight và rejection gates.
  Log `research/runs/0.3.22-vcpu-test.log`. VM/ROM PASS: `0.3.22-vm-test.log`.
- VZ300s vượtIRQ40 rồi dừngCR8read (SVMexit8) tạiRIP1fa59417, NRIP1fa5941b,
  decodeinfo8000000000000000. Teardown0/0/0. Log `0.3.22-vz-observe300.log`.
  ChưaLinuxboot. Đây là blocker mới, không phải lỗi frameIRQ cũ.

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


## Original Podman default machine recovery (2026-11-11)

User requested starting/fixing podman-machine-default. Docker was already stopped
(vms0/no Docker backend). Original12CPU/3814MiB applehv guest failed Ignition:
guest firstboot but host LastUp nonzero/Startingtrue omitted1024configvsock.
APFS-cloned pre-task93GiB disk and metadata/EFI/Ignition before repair. Restored
firstboot metadata, supplied existing config/identity. Initial tool-shell cleanup
interrupted provisioning; preserved intermediate disk and restored pre-task clone
before successful detached firstboot. No machine deletion or replacement image.
Also created missing host registries.conf with comment only; original Ansible
playbook rerun ok2/changed1/failed0, zero failed systemd units.
Two full default-machine12CPU workloads PASS, second after normal stop/start:
container/DNS/read-only bind/dedicatedvolume/uncachedbuild/CPU0..11 exacthashes/
localhost18081/API. Exact volume proof checked before second-run writes. Restart
cmdline no ignition.firstboot/no tsc=reliable. Collected restart dmesg no matching
unstable/softlockup/BUG/Oops pattern. Normalstop reachedvms0, restartvms1.
Final INTENTIONAL user handoff: defaultPodman RUNNING12CPU, API through both
private socket and /var/run/docker.sock OK; Docker stopped; fast/parallel/
takeover1 trace0, loaded40. No kext update/host reboot or sleep/wake test.
See research/PODMAN-DEFAULT-0.3.40.md and 0.3.40-podman-user-default-* logs.


## Workspace cleanup (2026-11-11)

Removed obsolete isolated fixvm-clean/fixvm-native machines and stale connections,
interrupted-init backup disk, installed Docker DMG and temporary caches. Preserved
clean2, original pre-repair backup, all source/audits/logs and active user VM data.
Default Podman remained running12CPU/API OK, vms1. Workspace ~11.37 to5.03GiB;
actual free disk +2.73GiB (APFS shared extents). See research/CLEANUP-2026-11-11.md.


## 0.3.41 review: sleep crash root cause, kick IPI, TLB reuse (2026-11-11)

Source review requested by user; default Podman VM stayed running on loaded40
(vms1) the whole time. No sysctl write, kext load, install or reboot in this turn.

- ROOT CAUSE of the 0.3.40 sleep hang: KDK disassembly of `acpi_sleep_kernel`
  calls `*hv_callbacks+0x10` (suspend) with only the enabled-flag test; 40 passed
  suspend/resume/memory_pressure = NULL. Any sleep with takeover1 jumps to 0.
  `hv_resume` null-checks; no kernel caller of memory_pressure found. 41 registers
  all eight callbacks; resume bumps a TLB flush epoch. Guest survival across S3
  is still UNTESTED.
- hv_vcpu_interrupt only set a pending bit; a vCPU inside the guest noticed at the
  next unrelated host IRQ. AppleHV `vcpu_kick` calls `mp_cpus_kick`. 41 resolves
  `mp_cpus_kick` (9th kernsyms entry, same UUID/KC, signature checked) and kicks
  the physical CPUs published by running vCPUs; Dekker handshake unit-tested.
- klog (mutex + IOLog) was called with host interrupts disabled at three sites,
  one of them per forwarded MSR in fast code64 (saved Podman log: 552 of 634
  retained lines were RDMSR 0x1b). Now deferred until IRQs are restored, MSR
  forwards limited to first64/vCPU unless trace_exits1.
- Every run call flushed the whole TLB (TLB_CONTROL1, ASID1 for all). 41: ASID
  2+slot*64+id per vCPU; flush (by ASID when FlushByAsid) only on first entry,
  physical CPU change, NPT generation change, outside change of CR0/CR3/CR4/EFER,
  emulated CR write/INVLPG, hv_vcpu_invalidate_tlb, resume epoch. Only in fast
  code64; every other mode and step still flush each run. ASID1 kept for selftest.
- Fast code64 no longer intercepts CR2/CR3 read/write or INVLPG; guest CR3 is
  read back from the VMCB on exit and revalidated at the next entry. PAUSE uses
  filter count3000/threshold128 in fast mode. Step gate policy unchanged.
- ~15 CPUID per run moved to hostCapsInit at takeover. VcpuState::index was a
  92-entry linear scan per get/set (566 cmpw in run()); now a compile-time table,
  verified equal to the old lookup for all 65536 encodings. vcpu_svm.o text
  10257 -> 6509 disassembly lines. FP publish/pull use bulk copies.
- New scripts/autostart.sh (LaunchDaemon, version-pinned, crash guard). Not
  installed. desktop-backend/fast-test/vcpu-test now require loaded41.
- Offline41 exit0: build/sign/imports346/ASan/UBSan/FP variants/resolver6/script
  syntax PASS. HARDWARE41 ALL PENDING: selftest, vm-test, vcpu-test --modes,
  fast-test serial+parallel, then Docker/Podman workloads and a deliberate
  sleep/wake test with no VM, then with an idle VM.
- Not done: in-kernel APIC (largest remaining gap vs AppleHV), hoisting the
  FP/XSETBV/VMSAVE switch out of the per-VMRUN path, in-kernel re-entry after
  host IRQ, VMCB clean bits.


## 0.3.42: 16 GiB VM start failure, FP switch per quantum, APIC finding (2026-11-11)

- User installed41, rebooted, backend enabled (fast1/parallel1). First hardware
  evidence for41, NOT a gate run: Docker 8 vCPU guest ran to Linux user space;
  per-vCPU runs12709..44805 with tlb_flushes988..1273 (3..8% of runs instead of
  every run), private ASIDs2..9. Log kept as
  research/runs/0.3.41-user-docker-8cpu-then-16g-fail.amdhv.log.
- User then set Docker to 8 CPU/16630 MiB: VZ "Internal Virtualization error. The
  virtual machine failed to start", Docker retrying (vm create then task exit, no
  vCPU). Cause: GuestMemory MaxPinned 8 GiB per VM (and MaxTotalPinned16, NPT
  MaxNodes8192, which a dense16 GiB map also exceeds). 42: one shared budget =
  max_mem - max(max_mem/4, 8 GiB); NPT bound131072 nodes. Unit test maps a single
  16 GiB region. Guest RAM is still fully wired at map time.
- In-kernel APIC NOT implemented, by finding: the x86 VZ service disassembly calls
  hv_vm_create with edi=0 and imports no hv_vm_lapic*/ioapic*/hv_vcpu_apic*
  symbol (only the arm64 hv_gic_* family). VZ emulates the APIC in user space on
  Intel as well, so a kernel APIC would never be called by Docker/Podman.
- FP: new amdhv_svm_fp_enter/leave (same macros as before) run once per quantum;
  amdhv_svm_vmrun_guest is the lean per-entry path (host VMSAVE only on the first
  entry; guest VMLOAD/VMSAVE and host VMLOAD every entry because the C loop needs
  host GS). Guest FP/XCR0 stay loaded across in-kernel exits with IRQs off; an
  effective guest XCR0 change (XSETBV, CR4.OSXSAVE) re-switches. amdhv_svm_vmrun_full
  removed. Expected gain limited to exits handled in-kernel.
- Offline42 exit0 (16 PASS lines, imports347). HARDWARE42 PENDING, in particular
  fpprobe/xsaveprobe in the fast and step gates and a16 GiB Docker start.
- User: no limits wanted, closest to AppleHV. Removed the RAM budget entirely
  (only bound: total wired <= max_mem) and the NPT node cap. Wiring all guest RAM
  at map time remains the one structural difference from AppleHV's demand paging.
- In-kernel continuation: run_until(forever) re-enters after a host-only exit
  (INTR/NMI/budget, no cancel, no EXITINTINFO) without pull/publish or a VMM round
  trip. Stops for a pending cancel (run() entry), hv_ast_pending (10th kernsyms
  entry) or a waiting exclusive gate writer (writersWaiting counter in Guard).
  Per-vCPU "continued in-kernel" count logged at destroy. hv_vcpu_run unchanged.

- Hardware42 (user gave sudo, test machine): installed, user rebooted, loaded42.
  selftest24/24, VM37, vCPU669, fast273 serial and273 parallel PASS lines, zeroFAIL,
  all exit0 (same counts as40). Logs research/runs/0.3.42-*-test.log.
- Docker Desktop8CPU/16630MiB (the configuration that failed on41) starts:
  engine29.9.3 reports8CPU/17.0GB. 8 parallel3GB sha256 streams identical hashes,
  10GB tmpfs write+hash, guest dmesg no lockup/stall/unstable/BUG/Oops match, kext
  log no stopped/rejected/refused line. Single session, not a soak test.
  Autostart daemon and sleep/wake still NOT tested.
