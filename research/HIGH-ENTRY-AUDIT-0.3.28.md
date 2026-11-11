# 0.3.28: high canonical kernel entry và EFER.SCE

2026-11-10. Loaded khi kiểm thử: AMDHV0.3.27.

## Chứng cứ runtime

- Gate thường vCPU --modes, VM --rom và fast-test đều PASS, không bật diagnostic.
  Gồm system descriptors, TR busy/nullSS, chuyển32→64, FPU, IRQ/IRET và timer.
  Logs `runs/0.3.27-vcpu-test.log`, `0.3.27-vm-test.log`, `0.3.27-fast-test.log`.
  Fast finite workload:20 physicalhostIRQ exits, worst call7.481ms trong lượt đo này.
- VZ Linux direct boot vượt LLDT/LTR và decompressor, thực thi CPUID trong kernel
  tạiffffffff810441b1; next entry ởffffffff810441b3 bị validator lower-half từ chối.
  CR0=80050033, CR3=2f1c000, CR4=20b0, EFER500; GSBASEffffffff82dee000;
  GDTffffffff82df9000, IDTffffffff82805000, null data/LDTR, TR busy hợp lệ.
  `runs/0.3.27-vz-linux60.log`. Start callback Running, rồi fail sau0.066s;
  console0bytes, cleanup sạch. **Kernel head đã thực thi, chưa Linux boot hoàn tất.**

## Rà rộng trước khi chốt bản

Đã giải nén payload gzip của đúng bzImage Alpine6.12.115-0-virt, đọc ELF PT_LOAD
để đối chiếu địa chỉ runtime. Byte CPUID0fa2 tạiffffffff810441b1 khớp ELF.
Rà cả block kernel entry, early IDT, initial_code pointerffffffff829ad6e8 và các
block gọi đầu tiên. `linux/alpine-high-entry-audit.txt` ghi hash/bytes/địa chỉ.
Đây là snapshot tĩnh các block chọn lọc, không phải toàn bộ call graph hay runtime trace.

Ngay sau blocker hiện tại, kernel đọc EFER, đặt SCE (bit0) và tùy CPUID đặt NXE,
WRMSR rồi gọi initial_code. Nếu chỉ sửa RIP/RSP thì SCE là giới hạn kế tiếp có thể
thấy trước từ cùng luồng mã. Đối chiếu nguồn chính:
[Linux kernel head_64.S](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S).

## Nhóm thay đổi0.3.28

| Nhóm | Thay đổi và kiểm tra |
|---|---|
| RIP/RSP64 | Cho cả hai nửa canonical48; vẫn loại canonical hole/LA57 và giới hạn mode16/32 cũ |
| PAUSE và internal flags breakpoint | Kiểm tra canonical cả RIP/NRIP; giữ tối đa15bytes, chống wrap và sentinel0 |
| Fetch lệnh sau exit | Walker vốn hỗ trợ48bit được kiểm thử thêm high aliases, NX và missing physical page |
| Software IRQ/IRET64 | Cho handler/stack/return address high canonical; giữ preflight atomic frame và canonical-hole rejection |
| SCE/NXE | Tách khỏi paging-mode bits; SCE chỉ chấp nhận trong fast runner, active IA-32e |
| SYSCALL state | VMCB/VMLOAD dùng guest STAR/LSTAR/CSTAR/SFMASK đã có allowlist; kiểm thử fast SYSCALL trong guestCPL0, không tuyên bố CPL3/SYSRET |
| Data cache64 | Giữ ring0/data type/base checks; cho D/B/G variations hợp lệ của cache do CPU tạo trong64bit |

SCE **vẫn bị từ chối trong step runner**: SYSCALL có thể xóa internalTF qua SFMASK,
phá giới hạn chạy. Chỉ runner được hostIRQ preempt mới cho bật SCE. Mọi state vẫn
được validate trước VMLOAD. Guest virtual addresses chỉ được dùng trong guest hoặc
walker→GPA; không cast thành host pointer. NPT owner/pins, physical limits, host
DR/EFER/MSR restoration và global VM lock không đổi. Không thêm CPL3, LA57, APIC/SMP
hay exception/NMI delivery đầy đủ.

## Kiểm thử

- Unit: mode high RIP/RSP, noncanonical hole, NRIP wrap/hole, high guest fetch/NX/missing
  page; high GDT/IDT/handler/stack software IRQ→IRET và frame crossing hole không ghi.
  Kiểm tra SCE bị từ chối trong step/legacy, chấp nhận fast IA-32e; reserved EFER vẫn fail.
- eventprobe mở rộng `--high`: high code+stack PAUSE/PUSHF/POPF/MOVDR, IRQ/IRET
  interrupt+trap gate, noncanonical RIP rejection, NPT fault cho high VA→low GPA.
  Step còn kiểm tra readonly frame/IST/NMI/badIRET và SCE rejection.
- Fast gọi `eventprobe --high --hardware`: kiểm tra native hardware IRQ/IRET, thêm
  SCE/SYSCALL dùng guest STAR/LSTAR/SFMASK, RCX/R11/full-width return và maskedflags.
  Không áp các rejection/preflight software-only lên hardware event delivery.
- Offline0.3.28 PASS, không warning, imports339, ASan/UBSan, FP preservation,
  resolver6cases, script syntax. `runs/0.3.28-offline-checks.log`.
- Các gate high-address/SCE mới **chưa chạy trên loaded0.3.28**; cần reboot.
  Không coi standard0.3.27 PASS hay unitPASS là hardware0.3.28 PASS.

Sau reboot: vCPU --modes → VM --rom → fast-test; chỉ khi tất cả PASS mới chạy lại
VZ Linux direct boot với assets Alpine riêng. Tiếp tục rà nhóm liên quan của lỗi mới.
Podman/Docker lifecycle/container/network/storage vẫn chưa đạt.
