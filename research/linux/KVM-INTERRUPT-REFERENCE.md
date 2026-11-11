# Đối chiếu KVM trên CachyOS cho nhóm guest interrupts (0.3.20)

Ngày 2026-11-10. Linux 7.3.3-1-cachyos, CachyOS, KVM API 12, kvm_amd npt=Y nrips=1 vgif=1 vnmi=Y avic=Y.
Ryzen 9 9900X (CPUID 1 eax=00b40f40), microcode 0x0b404038, SVM rev 1, 32768 ASID,
0x8000000A edx=febfbdff — khớp phần cứng/feature đã đo trên macOS ở phase 1.
Log đầy đủ: `2026-11-10-cachyos-7.3.3-sysinfo.txt`. Tái tạo: `research/linux/collect.sh` (không cần root).

**Giới hạn:** KVM PASS không chứng minh AMDHV hay Apple backend PASS. Đây là hành vi của
Zen 5 + KVM SVM, dùng làm đối chiếu thiết kế. Không có VM_CR/MSR (cần root, chưa đo).

## Bài test `kvm-ref/kvmref.c`

Guest 64-bit, CR0=80010033, EFER=d00 (giống trạng thái blocker DxeCore), GDT/IDT thật,
**không** có in-kernel irqchip: userspace tự inject như Apple VMM. 13/13 PASS, lặp 20 lần
trên 20 core khác nhau đều PASS.

| Test | Kết quả đo trên Zen 5 |
|---|---|
| irq-window-after-sti | `cli; sti; A: nop; B: nop` + request window → window mở tại **B** (shadow phủ A) |
| irq-inject-frame | IRQ 0x40 inject tại B: frame.rip=B, rflags=0x202 |
| tf-trace (guest tự bật TF) | #DB tại: sau POPF+1 lệnh, **sau STI có #DB**, **sau MOV SS không có #DB** (gộp 2 lệnh) |
| kvm-singlestep | sau STI `events.interrupt.shadow=3` (MOV_SS\|STI): SVM chỉ có 1 bit shadow, không phân biệt nguồn |
| hlt-if1 | HLT với IF=1 exit; KVM đã tự advance RIP qua HLT; IRQ đánh thức, frame.rip = sau HLT |
| inject #GP/#UD | #GP có error code 0x1234, frame.rip = RIP inject (fault); #UD không error code |
| NMI | trong handler nmi.masked=1, sau IRETQ masked=0 |

## Phát hiện áp dụng cho AMDHV (macOS)

Trạng thái code hiện tại (đã đọc nguồn): `validPilotControls` ép 0x4016=0 và primary ⊆ 0x200080;
`validPilotMode` ép 0x4824=0, RFLAGS ⊆ PilotFlags (không có IF); `VcpuSvm::run` bzero VMCB mỗi run,
`VINTR_MASKING=1`, chặn mọi exception, bật TF nội bộ cho từng bước.

1. **Shadow phải đi qua ranh giới run** (đo được). Khi quantum kết thúc ngay sau STI/MOV SS,
   VMCB `INT_SHADOW` (0x068 bit0) phải publish ra 0x4824 và nạp lại khi run tiếp. Hiện VMCB bị bzero
   → mất shadow → có thể inject sớm 1 lệnh.
2. **Ánh xạ shadow 1 bit → 0x4824** (đo + static). SVM không phân biệt STI/MOV SS (KVM báo 3).
   Apple VMM tự reject trước entry (`VirtualMachine-25G332.s` 0x10022C2D7…0x10022C499):
   cả hai bit cùng set; bit STI khi IF=0; bit ≥0x20; shadow + pending external interrupt;
   external interrupt khi IF=0. Đề xuất: shadow=1 → `IF ? 0x1 (STI) : 0x2 (MOV SS)`; không bao giờ
   publish 0x3. Chiều ngược: 0x4824 bit0|bit1 → VMCB shadow=1. (Suy luận; sai khác nhỏ: MOV SS còn
   chặn #DB/NMI — ghi rõ giới hạn.)
3. **Single-step + MOV SS chạy 2 lệnh/1 bước** (đo được). Budget 1024 đếm bước, không đếm lệnh;
   `flagsStepBoundary`/DR0 tại NRIP phải chịu được MOV SS/POP SS ngay trước PUSHF/POPF/PAUSE
   (lệnh thứ 2 vẫn bị intercept vì là instruction intercept). Nên thêm probe `mov ss; pushf`.
   STI không nuốt #DB → step sau STI dừng đúng tại lệnh kế (shadow vẫn =1 ở exit đó).
4. **IF=1 an toàn cho host** (suy luận APM, cần probe trên macOS): với V_INTR_MASKING=1 và host IF=0
   trong quantum, ngắt vật lý không vào guest; IF guest chỉ ảnh hưởng virtual/injected events.
   Đưa IF vào PilotFlags là phần nhỏ nhất gỡ blocker RFLAGS=206, **nhưng** cần kèm (1)(2) và
   từ chối injection trong shadow — không chỉ nới flag.
5. **Injection 0x4016/0x4018/0x401a → VMCB EVENTINJ 0x0A8** (đo type 0/2/3 qua KVM):
   vector 7:0, type 10:8 (VMX 0 ext→SVM 0, 2 NMI→2, 3 hw exc→3, 4 soft int→4), bit11 error valid,
   bit31 valid, error code ở 63:32. VMX type 5/6 (INT1/INT3/INTO) và soft int cần VMCB next_rip
   (0x0C8) = RIP + 0x401a; KVM làm qua `svm_update_soft_interrupt_rip` — chưa đo ở đây.
   Fault inject: frame.rip = RIP hiện tại (đo được). Sau exit, `EXITINTINFO` (0x088) → VMX IDT-vectoring
   info 0x4408/0x440a để VMM re-inject khi delivery bị NPF cắt ngang — chưa đo, cần cho EFI.
6. **Interrupt-window (primary bit2=0x4)** → V_IRQ=1 + V_IGN_TPR + intercept VINTR (word3 bit4),
   giống KVM `svm_set_vintr`; đo gián tiếp: window mở đúng sau shadow. Exit VMX reason 7.
   **NMI-window (primary bit22)** → Zen 5 có vNMI (đã có trong CPUID/KVM vnmi=Y); hoặc IRET intercept
   (AMDHV đã chặn IRET) + 1 step. NMI blocking (0x4824 bit3) phải theo dõi tới IRET (đo được).
7. **HLT khi IF=1**: KVM tự advance RIP; VMX/Apple ABI kỳ vọng exit 12 với RIP tại HLT và
   0x440c = độ dài (NRIP−RIP). Giữ nguyên quy ước VMX, không bắt chước KVM ở điểm này.

## Đề xuất phạm vi 0.3.20 (một lần cài/reboot)

- Cho phép IF trong PilotFlags; giữ shadow qua run (1)(2); validator chấp nhận 0x4824 ∈ {0,1,2,8,9,10}.
- EVENTINJ cho type 0/2/3 (+error code); reject type 4/5/6 rõ ràng đến khi có next_rip.
- Primary bit2 (interrupt-window) → VINTR exit 7; NMI-window để sau nếu VZ chưa dùng.
- EXITINTINFO → 0x4408/0x440a.
- Probe macOS mới: sti-shadow-window, movss-pushf boundary, inject #GP/#UD/ext-int, HLT IF=1,
  shadow qua quantum boundary; đối chiếu với bảng trên (cùng CPU).
