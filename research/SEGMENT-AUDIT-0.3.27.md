# 0.3.27: rà nhóm chuyển segment SVM sang VMX

2026-11-10. Loaded khi kiểm thử là0.3.26. Firmware hash không đổi so với lượt
inventory70 module trong `FIRMWARE-AUDIT-0.3.26.md`; tái sử dụng inventory đó,
không tuyên bố quét lại hoặc chứng minh toàn bộ code có thể chạy.

## Lỗi thật và nguyên nhân

Gate thường `vcpu-test.sh --modes` **FAIL** tại systemprobe, cả32/64bit:
LTR đặt GDT descriptor busy đúng, nhưng VMSAVE.TR attribute trả89 thay vì8b.
Đầu ra VMX này bị validator busy-TSS từ chối khi vào run kế tiếp, làm các kiểm thử
sau đó fail dây chuyền. Log: `runs/0.3.26-vcpu-test.log`.

Không nới validator để chấp nhận available-TSS trong VMCS. Cần chuyển đúng trạng thái
SVM đã lưu sang các invariant VMX. Chẩn đoán qua public framework API, chỉ trong probe
riêng, sửa89→8b sau exit và chạy tiếp. Điều này làm lộ thêm **null SS attribute4000**:
AMD để DB1 trong SS không present. 0.3.26 chỉ nhận diện attribute0 là unusable, do đó
run kế tiếp vẫn fail. Log `runs/0.3.26-system-diagnostic.log` ghi lượt chỉ sửaTR.

## Rà rộng nhóm liên quan

Đã đối chiếu bộ serialize cả8 caches với `svm_get_segment` của Linux/KVM:
[Linux SVM source](https://github.com/torvalds/linux/blob/master/arch/x86/kvm/svm/svm.c)
(đọc2026-11-10). Nguồn nêu cùng các trường hợp TR busy, G, data accessed,
not-present/unusable và DB của null SS. Áp dụng các invariant kiến trúc bằng helper
riêng, không sao chép code NVMM/KVM vào driver.

| Trường | Xử lý0.3.27 | Giới hạn giữ lại |
|---|---|---|
| TR type9 sau native LTR | Publish type11 nếu present system TSS32/64 type9 | TSS16/invalid type không được tự đổi thành supported type |
| Null SS còn DB1 | Cache không present thành VMX unusable10000 | Null CS, non-null unusable caches không được mở trong validator |
| DS/ES/FS/GS not present, LDTR null | Publish VMX unusable thống nhất | Canonical FS/GS base và CPL0 còn bắt buộc |
| Present data cache thiếu accessed | Khôi phục accessed invariant | Không mở rộng type/DPL/permissions |
| G bị bỏ trong saved cache | Helper chung giữ cơ chế suy ra G từ expanded limit trước đây | CS L/DB và mode validation không nới |

Helper mới `AMDHV/src/segment_attributes.hpp` dùng trực tiếp ở chỗ publish VMCB→VMCS
trong kext, và cùng helper được chẩn đoán trên máy thật qua userspace probe.
VMLOAD/VMSAVE host restoration, NPT ownership và input validators không đổi.

## Phân biệt lỗi fixture với lỗi kernel

Trong systemprobe64, GDTR=c000 nhưng guest PTEc000 chưa present, nên CPU tạo guest#PF,
không phải NPF. Đã tách thành hai ca: guest PTE nonpresent phải fail closed với RIP chưa
retire; sau khi cho guest PTE trỏ GPAc000 nhưng không có NPT backing, phải trảVMX48.
Probe32 vẫn kiểm tra NPF như trước. Không đổi kernel để biến guest#PF thành NPF.
Chưa thêm #PF injection hay exception delivery.

## Kiểm chứng trước reboot

- Chế độ chẩn đoán được gọi tường minh bằng `--diagnostic-normalize-segments`;
  header output ghi rõ có userspace sửa VMCS, không phải gate kext thường.
- Lượt chỉ sửaTR tìm được nullSSbug; lượt dùng helper đầy đủ PASS systemprobe32/64,
  gồm LTR/LLDT/STR/SLDT, NPT busy-write, busy-TSS fault, null data segments và cả hai
  đường guest#PF/NPF. `runs/0.3.26-segment-diagnostic.log`.
- Toàn bộ vCPU suite chạy lại với chỉ systemprobe/longtransitionprobe bật chế độ
  chẩn đoán: PASS, gồm TSS qua32→64 và các gate FPU/IRQ/CR/DR cũ.
  `runs/0.3.26-vcpu-diagnostic.log`.
- VM/ROM thường PASS: `runs/0.3.26-vm-test.log`.
- Fast suite với systemprobe chẩn đoán PASS. Timer finite workload:35 physicalhostIRQ
  exits, worst call5.297ms trong lượt đo này. `runs/0.3.26-fast-diagnostic.log`.
- Offline0.3.27 PASS, không warning, imports339, ASan/UBSan, FP preservation,
  resolver6cases, script syntax. `runs/0.3.27-offline-checks.log`.
- Unit tái hiện raw89 và nullSS4000 làm validator fail, rồi helper output làm đúng
  mode pass; kiểm tra untouched CS/G, busy TR, accessed và không mở TSS16.

**0.3.26 chưa PASS gate thường. Chẩn đoán trên bản cũ không xác nhận kext0.3.27.**
Không chạy VZ/Linux trên0.3.26 sau khi gate mới thất bại. Sau reboot cần chạy các script
thường, không có flag diagnostic: vCPU --modes → VM --rom → fast-test, rồi mới thử VZ
Linux direct boot. Linux/Podman/Docker chưa đạt.

ROM SHA256: 3b6ecc2b38e3004423bb260eb15322a970e70da26c11b17f0d64a290983f3903
