# Rà rộng firmware và Linux entry trước 0.3.26

Ngày 2026-11-10. Loaded khi kiểm thử: AMDHV 0.3.25, macOS 25G332.
Mục tiêu vẫn là Podman/Docker qua backend native macOS; chưa boot Linux thành công.

## Phạm vi và bằng chứng

- Đã chạy lại vCPU --modes, VM --rom, fast-test: tất cả PASS, gồm CR2 và CR4.DE.
  Fast finite loop có 14 host IRQ exits; worst call 8.542 ms trong lượt đo này.
- Quét **70 module PE**, đọc đúng PE executable section/RVA, theo machine32/64.
  Script: `tools/audit_efi_features.py`; kết quả: `research/efi-feature-inventory-25G332.json`.
  Đây là linear decode, không phải call-graph hay chứng minh reachability. Có thể gặp
  data/mã 16-bit nằm trong section64. TE/raw module không nằm trong inventory này.
  Không dùng số lệnh phát hiện để đếm số lỗi còn lại hoặc đánh giá phần trăm hoàn thành.
- Rà riêng CpuDxe: CR/DR, descriptor, x87/SSE save/restore, MSR/CPUID và cache/system.
  Danh sách site: `research/efi-cpu-state-audit-25G332.txt`.
- Rà luồng IRQ save/restore đã biết, gồm CR8/CR4.DE/CR3/CR2/CR0/DRx/FXSAVE/FXRSTOR/IRET.
  Các gate CR2/IRQ/IRET đã PASS0.3.25; không suy ra toàn bộ firmware đã hoàn tất.
- Thêm delegate guest-stop, console và elapsed time vào vzprobe, chỉ thay userspace.
  EFI có serial port vẫn tự dừng sau0.227s, guest-stop callback có thật, console0bytes.
  `research/runs/0.3.25-vz-console60.log`. Lần EFI không console cũng tự dừng, không có
  kext Unsupported ở đuôi log. Không coi trạng thái Stopped hay Running là boot proof.
- Tìm đúng ResetSystemRuntimeDxe, inferred base1fbaf000, khớp cả bốn IO RIP cuối:
  RVA12c1/12cd/12cf/12d6. Mã đọc/ghi port604, đặt sleep-type rồi sleep-enable.
  `research/efi-shutdown-25G332.txt`. Đây là đường guest shutdown, không phải bằng
  chứng backend bị crash; **chưa xác định caller/nguyên nhân firmware chọn shutdown**.

## Thử thêm Linux để tìm lỗi liên quan trước reboot

Đã thêm chế độ `vz-probe.sh --fast --linux KERNEL INITRD --seconds N` bằng
VZLinuxBootLoader, cùng backend Apple; không dùng QEMU/TCG, không mở disk Podman.
Tải riêng Alpine v3.22 netboot kernel6.12.115-0-virt và initramfs từ mirror chính thức.
Nguồn/hash lưu ở `research/assets/alpine-v3.22/SOURCE.txt`; hash là ghi nhận cục bộ,
không phải xác minh chữ ký upstream. VM1CPU/512MiB, không disk/network.

Linux direct entry bị chặn ở LLDT AX tại10000f5 (SVM6c), NRIP10000f8.
`research/runs/0.3.25-vz-linux60.log`; không có console/kernel boot proof.
Rà mã bzImage quanh điểm này tìm được **LTR AX ngay sau LLDT**, rồi CR0.PG/far return
sang64bit. Đầu64bit tiếp tục **xóa cả DS/ES/SS/FS/GS**. Nếu chỉ sửa LLDT thì các
validator/cache cũ vẫn chặn những thao tác liên quan ngay sau đó.
`research/linux/alpine-direct-entry-audit.txt` ghi offset, bytes và decode.

## Các nhóm được gom vào 0.3.26

| Nhóm | Bằng chứng | Thay đổi/kiểm chứng |
|---|---|---|
| LLDT và LTR | Linux LLDT10000f5/LTR10000fd; CpuDxe LTR RVA1117 | Cho CPU thực thi qua guest GDT/NPT; vẫn intercept task switch/exceptions |
| LDTR/TR hidden cache | LTR/SLDT/STR, IRQ và mode re-entry | Nạp/lưu selector/base/limit/AR từ VMCS ↔ VMCB, giữ cache qua mỗi run |
| Null LDTR | Linux LLDT AX với AX0 | Map SVM attribute0 thành VMX unusable; validate null cache riêng |
| Null data segments trong code64 | Linux1000204..100020c | Cho null DS/ES/SS/FS/GS, giữ canonical FS/GS base; không mở CPL3 hay null CS |
| Protected32 → compatibility → code64 | Linux100010e..100020e | Mở rộng longtransitionprobe để nạp TR/clear LDTR trước PG và kiểm tra cache còn nguyên sau far jump |
| Descriptor faults | GDT/TSS có thể unmapped/readonly/busy | Hardware gate mới kiểm tra NPT write-protect busy-bit, busy TSS rejection, unmapped GDT |

Đã audit assembly: VMLOAD guest/VMSAVE guest nằm trong GIF0; VMLOAD host khôi phục
TR/LDTR/FS/GS trước trả C++ và bật host IRQ. Không dùng LTR/LLDT với selector guest
trên host; CPU chỉ thực thi lệnh trong guest. Guest GDT busy write chịu NPT.
Validator chỉ chấp nhận LDT và busy32/64TSS trong phạm vi hiện tại, canonical base,
loại bỏ TI/invalid attributes/available TSS cache/limit quá nhỏ. Không hỗ trợ task switch,
CPL3, exception/NMI delivery đầy đủ, high canonical RIP/RSP hay OSXSAVE.

## Khoảng trống còn thấy qua rà tĩnh

| Nhóm | Trạng thái |
|---|---|
| CR/DR, FX save/restore, IRQ/IRET cũ | Gate phần cứng PASS0.3.25 trong phạm vi đã hỗ trợ |
| WBINVD/INVD, MONITOR/MWAIT | Có site trong CpuDxe; chưa chứng minh reachable trên đường hiện tại; vẫn fail closed, không chạy trên host |
| MSR/APIC/timer và ngoại lệ | MSR có allowlist; APIC và event chỉ subset; cần tiếp tục theo chứng cứ runtime |
| Linux kernel entry sau decompressor, CPL3/SMP | Chưa xác minh; các giới hạn địa chỉ, controls, vector state vẫn còn |
| Podman/Docker lifecycle/network/storage | Chưa đạt; không suy ra từ EFI hoặc kernel head |

Không hứa hết lỗi sau reboot. Mục đích lượt rà này là gom các lỗi có liên hệ đã nhìn
thấy và kiểm thử cả nhóm trước cài, thay vì mở rộng mọi tính năng từ một inventory tĩnh.

## Kiểm thử và lượt reboot kế tiếp

Offline0.3.26 PASS, không warning, imports339, ASan/UBSan, FP preservation,
resolver6cases và script syntax. Log `research/runs/0.3.26-offline-checks.log`.
Hardware mới còn pending: systemprobe32/64 trong step và fast; longtransitionprobe
mới trong step. Chỉ thử VZ tiếp nếu các gate này PASS.

Sau reboot0.3.26:
1. `scripts/vcpu-test.sh --modes`
2. `scripts/vm-test.sh --rom`
3. `scripts/fast-test.sh`
4. `scripts/vz-probe.sh --fast --linux /Users/phucduong/Desktop/FixVM/research/assets/alpine-v3.22/vmlinuz-virt /Users/phucduong/Desktop/FixVM/research/assets/alpine-v3.22/initramfs-virt --seconds 60`
5. Rà lỗi mới và thử EFI có boot media nếu cần; chưa coi EFI guest-stop là đã xong.

ROM SHA256: 3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903
