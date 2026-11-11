# 0.3.29: synchronous guest faults và VMX exception ABI

2026-11-10. Loaded khi kiểm thử: AMDHV0.3.28.

## Runtime và rà luồng kernel

- Gate thường vCPU --modes, VM --rom, fast-test đều PASS; gồm high code/stack,
  software/hardware IRQ/IRET, SCE/SYSCALL. Logs `runs/0.3.28-{vcpu-test,vm-test,fast-test}.log`.
  Fast finite workload:24 physical host IRQ exits, worst call6.361ms trong lượt đo này.
- VZ Linux tiến tiếp sau SCE enable, dừng tại REP MOVSD ởffffffff82e3c985 với
  SVM#PF exit4e, error0, faultVAffff888000001000. Đúng instruction bytesf3a5 đã có
  trong ELF audit `linux/alpine-high-entry-audit.txt` (kernel boot-data copy block).
  `runs/0.3.28-vz-linux60.log`; fail sau0.068s, console0bytes, teardown sạch.
  Kernel initialization đã chạy, chưa Linux boot hoàn tất hoặc container proof.
- Đây là guest page fault do page table guest, không phải NPT ownership failure.
  Backend cũ intercept mọi exception và trả Unsupported, khiến handler của Linux
  chưa có cơ hội xử lý. Không đổi #PF thành NPF hoặc bỏ kiểm tra backing.

## Nhóm triển khai

| Đường | 0.3.29 |
|---|---|
| Native fault trong fast code64 | CPU chuyển vào guest IDT cho DE/UD/NM/DF/TS/NP/SS/GP/PF/MF/AC/XM nếu bitmap không yêu cầu exit |
| Exception bitmap yêu cầu chặn | Trả VMX reason0, valid/type/vector, error-code flag và4406; #PF có faultVA trong exit qualification |
| VMCS reinjection | Nhận hardware exception type3 của nhóm trên, validate error flag/width, ghép4018 vào EVENTINJ high32; one-shot và vectoring metadata giữ lại |
| #PF và CR2 | Native delivery dùng CR2 của CPU; MOV CR2 trong handler đọc VMCB mới nhất, không snapshot cũ. Direct VMX#PF exit giữ CR2 trước entry, faultVA chỉ nằm ở qualification |
| Page fault filter | Hỗ trợ mask0: match0 theo bitmap, match khác0 đảo ý nghĩa bit14. Mask khác0 cần dynamic filter/reflection, hiện reject trước entry |
| Fault khi đang vào handler | NPT exit vẫn VMX48, interrupted-event metadata qua4408/440a; guest shutdown do triple fault thành VMX reason2 |
| RF | Cho guest RF trong fast runner để xử lý fault return; step vẫn giữ restrictions cũ |

Phạm vi chỉ fast IA-32e code64 đã validate, không mở guest TF/debug/NMI/machine-check,
reserved/vendor exceptions, CPL3 hoặc LA57. Step runner vẫn intercept/reject exception
để giữ internalTF budget. VCPU IRQ/host FPU/MSR restoration, physical ownership và NPT
quyền ghi vẫn giữ nguyên. Native fault handler vẫn chịu host IRQ preemption.
Không tuyên bố full exception architecture hay SMP đã hoàn tất.

## Đối chiếu nguồn và ABI

Đã đọc các site exception bitmap4004 trong Apple VMM25G332 (gồm BFEF1/BFF09,
C0301/C0319 và222ADE/222D70/224338): debugger/nested paths có thay bitmap,
vì vậy không được bỏ qua yêu cầu intercept của caller.

[Intel SDM Vol3C](https://cdrdv2-public.intel.com/671506/326019-sdm-vol-3c.pdf)
phân biệt direct exception VM-exit với fault trong khi delivery; direct#PF không
cập nhật CR2. [Intel PF mask/match semantics](https://cdrdv2-public.intel.com/774497/326019-sdm-vol-3c.pdf)
xác định cách bitmapbit14 được đảo khi mask/match không bằng nhau. Không suy ra
Intel ABI bằng cách copy nguyên SVM exit number hoặc cập nhật CR2 ở mọi exit.

## Kiểm thử và giới hạn bằng chứng

- Pure unit cover fault-vector/error-code classification, SVM intercept mask,
  hai trường hợp constant PF filter, entry type/error-code validation, EVENTINJ
  error bits và RF fast/step guards.
- faultprobe mới với guest GDT/IDT/page tables/stack và code high-half thật:
  #PF handler ghi PTE, reloadCR3 rồi IRET/retry read; #UD handler skipUD2;
  #GP handler nhận selector error và sửa return RIP.
- Kiểm tra VMX0 bitmap cho PF/UD/GP, CR2 không đổi trên direct#PF exit,
  inject lại PF/UD/GP với error0/nonzero; constant PF filter inversion;
  reject dynamic mask; NPT fault trong UD delivery; guest triple fault exit2.
- systemprobe giữ các fault negative dưới bitmap khi fast, cập nhật kỳ vọng
  VMX0 cho code64; step/fast32 vẫn kỳ vọng Unsupported như trước.
- Offline0.3.29 PASS, không warning, imports339, ASan/UBSan, FP preservation,
  resolver6cases và script syntax. `runs/0.3.29-offline-checks.log`.
- **Các gate fault mới và kext0.3.29 chưa được kiểm chứng phần cứng.** Không chạy
  chúng trên bản0.3.28 rồi coi kết quả là0.3.29. Cần reboot để nạp kext mới.

Sau reboot: vCPU --modes → VM --rom → fast-test (faultprobe ở cuối). Chỉ khi tất cả
PASS mới tiếp tục VZ Linux direct boot. Podman/Docker/container chưa đạt.
