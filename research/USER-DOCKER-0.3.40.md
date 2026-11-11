# Dùng thử Docker Desktop với AMDHV0.3.40

Docker Desktop4.99.0 đã được cấu hình Apple Virtualization,4CPU/4GiB/virtiofs.
Các lượt kiểm thử dùng backend thật, container/build/DNS/port/bind/volume và hash
trên cả4CPU. Chế độ song song đã qua kiểm thử phần cứng và workload lặp lại.
Đây là cấu hình dùng thử; kiểm thử dài hạn chưa hoàn tất.

**Sleep/wake chưa đạt kiểm chứng:** host đã tự Idle Sleep lúc12:10:22 trong bài
test headless, sau đó người dùng phải hard restart vì không thức lại được. Khi
dùng thử VM, giữ máy thức bằng `caffeinate -i` trong một Terminal; Ctrl-C sau khi
quit Docker. Agent dùng assertion tạm trong phiên kiểm thử; các script probe cũng
tự giữ máy thức đến khi chạy và dọn xong. Đây là cách tránh idle sleep khi thử,
không phải bản sửa lỗi sleep/wake hoặc bằng chứng nguyên nhân nằm trong kext.

## Mở ứng dụng

Sau khi agent báo backend đã sẵn sàng, mở Docker Desktop bình thường. Podman
Desktop/machine đang dừng để bạn thử riêng Docker. Không cần mở script probe;
các script đó tự dừng engine sau khi kiểm thử.

Nếu đã reboot host hoặc backend bị tắt, từ thư mục dự án chạy:

```sh
sudo scripts/desktop-backend.sh start
```

Helper yêu cầu bản0.3.40 đang nạp, bật fast/parallel/takeover và tắt log đầy đủ.
Nó không mở VM hoặc thay đổi cấu hình boot. Khi backend đã bật đúng cấu hình,
chạy lại helper không khởi động lại engine đang dùng. Sau reboot host cần bật
lại; hiện chưa có cơ chế tự bật AMDHV khi mở ứng dụng.

## Kiểm tra CLI

```sh
scripts/desktop-backend.sh status
docker --context desktop-linux info
docker --context desktop-linux run --rm alpine:3.22 echo FIXVM_DOCKER_OK
```

Socket Docker đã kiểm chứng là `~/.docker/run/docker.sock`. Alias toàn cục
`/var/run/docker.sock` hiện thuộc helper Podman; dùng context desktop-linux để
chọn đúng engine. Cấu hình chia sẻ file trong đợt test hiện chỉ chứa thư mục
`research/disposable/shares/Users`; thêm thư mục cần dùng qua Docker Settings
khi thử project/bind mount khác.

## Dừng

Quit Docker Desktop, chờ VM dừng, rồi nếu muốn tắt backend:

```sh
sudo scripts/desktop-backend.sh stop
```

Helper từ chối tắt khi còn VM. Kext vẫn nạp; tắt backend trả hv_support/fast/
parallel/takeover về0. Đổi chế độ parallel yêu cầu dừng mọi VM và tắt takeover,
không cần reboot host.

Kết quả và giới hạn chi tiết: `NATIVE-ACCEPTANCE-0.3.40.md` và PROCESS.md mới nhất.
