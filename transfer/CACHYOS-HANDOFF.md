# FixVM — bàn giao sang CachyOS (2026-11-10)

Đọc PROCESS.md và PLAN.md trước khi làm việc. Đây là bản sao dự án từ
/Users/phucduong/Desktop/FixVM trên macOS. Đích người dùng yêu cầu: ổ Data/Code/FixVM.
Không chứa mật khẩu sudo; hỏi lại trên Linux nếu cần.

## Mục tiêu và checkpoint

Mục tiêu: Podman applehv/vfkit và Docker Desktop dùng backend native macOS trên AMD.
Linux/KVM chỉ là môi trường đối chiếu, không thay thế tiêu chí hoàn thành.
Máy Ryzen 9 9900X, macOS26.7.2 build25G332, AMDHV0.3.19 đã loaded.
Theo PROCESS: 0.3.19 vCPU --modes và VM --rom PASS. VZ observation300s chưa boot,
dừng ở validator vì RFLAGS206 (IF=1), RIP1ffb1d9d, CR0=80010033, CR3=1fc01000,
CR4=2660, EFER=d00. Không còn blocker PAUSE/PUSHF trước đó.
Bước tiếp theo: nghiên cứu guest interrupts, STI/MOV SS shadow, VM-entry injection,
interrupt-window/NMI-window, exceptions và HLT IF=1. Không chỉ nới PilotFlags rồi
kết luận đã hỗ trợ ngắt. Chi tiết/log nằm trong PROCESS.md và research/runs/.

## Công việc được yêu cầu trên CachyOS

1. Xác minh hệ điều hành, vị trí Data và dự án. Giữ nguyên phân vùng/VM Podman cũ.
2. Thu thập CPU/CPUID, microcode, SVM/NPT, kernel/KVM version và log KVM.
   Lưu kết quả vào research/linux/ với ngày, kernel version và lệnh tái tạo.
3. Kiểm tra /dev/kvm và dựng kiểm thử KVM nhỏ làm đối chiếu cho trạng thái CPU,
   IF/shadow, exception và event injection. Xác minh công cụ/dependency trước.
4. So sánh với thiết kế AMDHV và ghi lại phát hiện phục vụ triển khai trên macOS.
   KVM PASS không chứng minh AMDHV hoặc backend Apple PASS.
5. Cập nhật PROCESS/PLAN trước khi quay lại macOS. Không cài kext trên Linux.

## Cách tiếp tục phiên làm việc

Mở thư mục Data/Code/FixVM trong công cụ coding trên Linux và dùng lời nhắn:
"Đọc transfer/CACHYOS-HANDOFF.md, PROCESS.md và PLAN.md. Tiếp tục thu thập dữ liệu
và dựng kiểm thử đối chiếu KVM cho dự án AMDHV trên CachyOS."

Bản sao project không mang theo lịch sử chat hoặc quyền truy cập phiên của ứng dụng.
Nếu cùng tài khoản có cuộc trò chuyện cũ, có thể mở lại; nếu không, dùng handoff này.
Build hiện tại là Mach-O/macOS, không chạy trên Linux. Python venv của macOS không
được đóng gói; tạo môi trường Linux riêng nếu cần capstone/uefi_firmware.
Không sửa firmware/bootloader hay format phân vùng để chuyển dữ liệu.

## Tình trạng chuyển dữ liệu

Ổ Data là NTFS, UUID43C00220-AF65-4821-96C2-E26415085DB6.
Đã dùng NTFS-3G2022.10.3 + FUSE-T1.2.9 để mount đọc/ghi trên macOS.
Bản project đặt ở Data/Code/FixVM. Trên Linux tìm mountpoint của Data bằng lsblk -f;
đường dẫn có thể là /run/media/<user>/Data/Code/FixVM.
Bỏ qua .venv-research (môi trường riêng macOS) và .DS_Store khi chép.
Bản gốc macOS giữ nguyên. Không có thay đổi AMDHV trong bước chuyển dữ liệu.
