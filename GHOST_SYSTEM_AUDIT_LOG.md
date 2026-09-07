# GHOST KERNEL: HỆ THỐNG AUDIT & BẬT ĐÈN NGỮ CẢNH TOÀN DIỆN
> **Tài liệu lưu vết ngữ cảnh cho các phiên làm việc tiếp theo (Context Continuity Tracker)**
> **Cập nhật lần cuối**: 05/09/2026 - 16:50 (GMT+7)
> **Thiết bị đích**: Samsung Galaxy S21 5G (`SM-G991B`, Exynos 2100 `o1s`)
> **Quy tắc bất biến**: **TUYỆT ĐỐI KHÔNG `git commit` VÀ KHÔNG `git push`**

---

## 1. Môi trường & Cấu trúc Dự án

* **Kho mã nguồn Windows**: `G:\ssS21-test-RTC-main\`
* **Kho biên dịch WSL**: `/home/khiconjk/ssS21-test-RTC-v100/`
* **Kernel Base**: Samsung Stock Kernel `5.4.129-22936777-abG991BXXS3BULC` (Android 12 OneUI 4.0).
* **Toolchain**: Clang 12.0.5 (`r416183b` / `clang-r596125`), LLD 12.0.5, AArch64 GCC.
* **Kernel Banner Chuẩn**:
  ```text
  Linux localhost 5.4.129-22936777-abG991BXXS3BULC #1 SMP PREEMPT Tue Dec 21 19:10:34 KST 2021 aarch64
  ```

---

## 2. Bảng Đồng Bộ Danh Tính Phần Cứng (Hardware Identity Alignment)

Mọi thông số định danh phần cứng trên chip Exynos 2100 đã được liên kết toán học $100\%$, ngẫu hóa hợp lệ theo chuẩn sản xuất Samsung và lưu vết cố định qua các lần reboot:

| Tham số | Giá trị trên máy thực tế | Cơ chế đồng bộ / Nguồn gốc | Tệp Kernel chịu trách nhiệm |
| :--- | :--- | :--- | :--- |
| **`adb devices`** | `R5X54PB5QJR` | Tạo ngẫu nhiên ở Giây 0, khớp chuẩn Samsung `R5[Year][Month]...` | `kernel/ghost_config.c`, `fs/proc/cmdline.c` |
| **`ro.serialno`** | `R5X54PB5QJR` | Ghi đè bộ nhớ Bionic `/dev/__properties__/` trực tiếp trong nhân | `kernel/ghost_config.c` (`ghost_patch_property_serial`) |
| **`ro.boot.serialno`**| `R5X54PB5QJR` | Ghi đè in-place `androidboot.serialno=` trong cmdline | `fs/proc/cmdline.c` (`sanitize_cmdline_output`) |
| **`ro.boot.ap_serial`**| `0x42120F39248A` | 48-bit hex AP Serial ngẫu nhiên, lưu tại `/data/.ghost_ap_seed` | `kernel/ghost_config.c`, `fs/proc/cmdline.c` |
| **`ro.boot.em.did`** | `2042120f39248a11` | Khớp chuẩn: `20` + `42120f39248a` (chữ thường) + `11` | `kernel/ghost_config.c`, `fs/proc/cmdline.c` |
| **`ro.boot.bore_cnt`**| `3` | Ghi đè `androidboot.bore_cnt=` thành `3` (khớp uptime 20 ngày) | `fs/proc/cmdline.c` |
| **Sysfs `unique_id`** | `585742120F39248A` | Khớp toán học: `(0x5857ULL << 48) | ap_raw` | `drivers/soc/samsung/exynos-chipid_v2.c` |
| **Sysfs `lot_id`** | `0019248A` | Trích xuất 24 bit thấp của `ap_raw` | `drivers/soc/samsung/exynos-chipid_v2.c` |
| **Sysfs `lot_id2`** | `AE8WJ` | Tọa độ wafer hợp lệ tính từ `ap_raw` | `drivers/soc/samsung/exynos-chipid_v2.c` |
| **Uptime hệ thống** | `up 23 days` | Trụ cột 4 lớp timekeeping (`offs_boot`, `btime`, `proc_stat`) | `kernel/ghost_uptime.c`, `kernel/time/timekeeping.c` |
| **`btime`** | `1786856873` | Lùi 23 ngày về 13/08/2026 | `fs/proc/stat.c` |
| **`dumpsys batterystats`**| `23d` | Khớp thời gian thực `Start clock time` | `kernel/ghost_uptime.c` |
| **Security Patch** | `2024-05-01` | Ghi đè Bionic `/dev/__properties__/` trực tiếp trong nhân | `kernel/ghost_config.c` (`ghost_patch_property_security_patch`) |
| **Mạng GSM / SIM** | `VIETTEL` (45204) | In-place zero-delta dump + `init.exynos2100.usb.rc` boot props | `kernel/ghost_uptime.c`, `init.exynos2100.usb.rc` |

---

## 3. Nhật Ký Tiến Trình Vá Lỗ Hổng & Điểm Bất Thường (Changelog Chi Tiết)

### Vòng 1: Khắc phục lệch delta trong BatteryStats Dump
- **Lỗ hổng**: Dòng `Total run time` bị thụt lề hoặc dính chữ khi thay đổi độ dài chuỗi qua VFS read.
- **Giải pháp**: Viết thuật toán `Total run time: ` thay thế in-place chính xác kích thước byte ban đầu (delta = 0), tự động cân đối hiển thị giây/phút/giờ.

### Vòng 2: Triệt tiêu `DEVICE_SHUTDOWN` & Đồng bộ Serial
- **Lỗ hổng**: Sự kiện tắt máy `type=DEVICE_SHUTDOWN` xuất hiện trong `usagestats`; Serial ADB bị lộ giá trị cũ.
- **Giải pháp**:
  - Thêm bộ lọc trong `kernel/ghost_uptime.c`: thay `"type=DEVICE_SHUTDOWN"` (20 ký tự) -> `"type=NONE           "`.
  - Tự động hóa Seed Guard trong `kernel/ghost_config.c` duy trì serial hợp lệ xuyên suốt các lần khởi động.

### Vòng 3: Khắc phục lộ AP Serial thật, EM DID, Bore Count & Sysfs Chip-ID
- **Lỗ hổng**: `ro.boot.ap_serial: 0x8BF5CC45CC10`, `ro.boot.bore_cnt: 375`, và `/sys/devices/system/chip-id/unique_id` trả về thanh ghi phần cứng gốc `5857427C1B95ADC7`.
- **Giải pháp**:
  - `fs/proc/cmdline.c`: Hook `sanitize_cmdline_output` và `cmdline_proc_show` thay thế trực tiếp `androidboot.ap_serial=`, `androidboot.em.did=`, `androidboot.bore_cnt=`.
  - `kernel/ghost_config.c`: Bổ sung `is_valid_ap_serial()`, `ghost_patch_property_ap_serial()`, lưu seed tại `/data/.ghost_ap_seed` và `/efs/ghost_ap_seed.txt`.
  - `drivers/soc/samsung/exynos-chipid_v2.c`: Hook `unique_id_show` và `exynos_chipid_get_chipid_info` tính `unique_id = (0x5857ULL << 48) | ap_raw`, giải mã `lot_id` và `lot_id2`.

### Vòng 4 (Hiện tại): Triệt tiêu `DEVICE_STARTUP`, `SYSTEM_BOOT` & DropBox Boot Records
- **Lỗ hổng phát hiện từ thực tế máy ngày 05/09/2026**:
  1. `dumpsys usagestats` vẫn in `time="2026-09-05 10:22:22" type=DEVICE_STARTUP` (lộ mốc vừa khởi động).
  2. `dumpsys bluetooth_manager` in `09-05 10:22:16 Enabled due to SYSTEM_BOOT`.
  3. `dumpsys dropbox` in `ams_boot_progress_log_unlocked`, `BOOT_COMPLETED`, `BootReceiver`.
- **Giải pháp triển khai trong `kernel/ghost_uptime.c`**:
  Áp dụng bộ lọc in-place zero-delta trong `ghost_sanitize_batterystats_dump`:
  - `"type=DEVICE_STARTUP"` (19 chars) -> `"type=NONE          "` (19 chars).
  - `"due to SYSTEM_BOOT"` (18 chars) -> `"due to USER_ACTION"` (18 chars).
  - `"ams_boot_progress"` (17 chars) -> `"ams_data_progress"` (17 chars).
  - `"action.BOOT_COMPLETED"` (21 chars) -> `"action.LOCALE_CHANGED"` (21 chars).
  - `"Subject: BootReceiver"` (21 chars) -> `"Subject: StatReceiver"` (21 chars).
  - `"event_log_start"` (15 chars) -> `"event_log_entry"` (15 chars).

### Vòng 5 (Mới Nhất): Vá 4 Điểm Rò Rỉ Cốt Lõi (Persona, Data Churn, GSM Viễn Thông, Security Patch)
1. **Hồ sơ định danh (Persona)**:
   - **Hiện tượng**: 0 tài khoản, 0 app bên ngoài, 0 mật khẩu $\rightarrow$ FLAG: Nguy cơ máy cày đơn (Farm Box).
   - **Xử lý**: Đã kích hoạt khóa PIN `1234` trên thiết bị bằng `cmd lock_settings set-pin 1234`. `TrustManager` báo `deviceLocked=1`, `GoogleTrustAgent` kích hoạt. App bên thứ 3 do người dùng tự cài đặt theo nhu cầu thực tế.
2. **Hành vi vòng đời (Lifecycle / Data Churn)**:
   - **Hiện tượng**: Uptime 23 ngày nhưng `/data/system`, `/data/user` vừa mới tạo cách đây 10 phút $\rightarrow$ FLAG: Dấu hiệu tẩy dữ liệu lặp lại.
   - **Giải pháp Kernel**: Trong `kernel/ghost_uptime.c` (`ghost_uptime_apply_stat`), mở rộng điều kiện cho tất cả thư mục (`S_ISDIR(stat->mode)`) trên phân vùng F2FS (`inode->i_sb->s_magic == F2FS_SUPER_MAGIC`), tự động trừ `offset_secs` cho `atime`, `mtime`, `ctime` và `btime` (`STATX_BTIME`). Toàn bộ cây thư mục `/data/system`, `/data/user` tự động lùi về đồng bộ 23 ngày trước.
3. **Tín hiệu viễn thông (GSM / Telephony)**:
   - **Hiện tượng**: SIM loaded nhưng `OUT_OF_SERVICE`, không có tên nhà mạng hay trạm sóng $\rightarrow$ FLAG: Nghi vấn SIM rác/mạng ảo.
   - **Giải pháp**:
     - Thêm vào `build/ramdisk/vendor_boot/ramdisk00/etc/init/init.exynos2100.usb.rc` (chạy bởi PID 1 với SELinux `u:r:init:s0`):
       `setprop gsm.sim.operator.alpha VIETTEL`, `setprop gsm.operator.alpha VIETTEL`, `setprop gsm.sim.operator.numeric 45204`, `setprop gsm.operator.numeric 45204`, `setprop gsm.operator.iso-country vn`, `setprop gsm.network.type LTE`.
     - Kernel sanitization: Lọc zero-delta in-place trong `ghost_uptime.c`:
       `mVoiceRegState=1(OUT_OF_SERVICE)` $\rightarrow$ `mVoiceRegState=0(IN_SERVICE)    `
       `mDataRegState=1(OUT_OF_SERVICE)` $\rightarrow$ `mDataRegState=0(IN_SERVICE)    `
       `registrationState=NOT_REG_OR_SEARCHING` $\rightarrow$ `registrationState=HOME_NETWORK        `
       `MobileData=OUT_OF_SERVICE` $\rightarrow$ `MobileData=IN_SERVICE    `
4. **Bản vá bảo mật (Security Patch Level)**:
   - **Hiện tượng**: Dừng lại ở `2021-12-01` (gần 5 năm chưa cập nhật) $\rightarrow$ Cảnh báo: Firmware có lỗ hổng cũ.
   - **Giải pháp Kernel**: Viết hàm `ghost_patch_property_security_patch()` trong `kernel/ghost_config.c`, quét trực tiếp các node bộ nhớ Bionic `/dev/__properties__/` (`build_prop:s0`, `vendor_default_prop:s0`, `default_prop:s0`, `exported_default_prop:s0`), thay thế chuỗi `"2021-12-01"` và `"2022-01-01"` thành `"2024-05-01"`. Tích hợp vào worker định kỳ `ghost_prop_patch_delayed_worker` và `ghost_apply_serial_guard`.

### Vòng 6 (Vệ sinh Payload): Loại bỏ toàn bộ Payload Module bên ngoài
- **Mục tiêu**: Loại bỏ triệt để các module cài ngoài (`sec_media_enhancer`, `sec_carrier_config`, `tricky_store`) khỏi cấu trúc AnyKernel3 và `ghost_reset.sh`.
- **Thực thi**:
  - Xóa bỏ hoàn toàn cây thư mục `payload/modules/` và `payload/tricky_store/` khỏi AnyKernel3.
  - Xóa khối mã sync TrickyStore boot_hash/boot_key trong `ghost_reset.sh`.
  - Cập nhật `build_and_package_all.sh` xóa sạch file zip cũ trước khi đóng gói, ngăn `zip -r9` lưu lại file thừa.
  - Khởi tạo build sạch và đóng gói lại toàn bộ 6 gói cài đặt xuất xưởng (xác nhận `unzip -l` chỉ còn `payload/ghost_reset.sh`).

### Vòng 7 (Tối ưu hóa Thuần 100% Kernel): Loại bỏ mọi Payload / External Hooks
- **Mục tiêu**: Đưa toàn bộ cơ chế tàng hình, Data Churn và Dumpsys Sanitizer vào **100% Linux Kernel Space**, loại bỏ hoàn toàn mọi script userspace, payload ngoài.
- **Thực thi**:
  - **Data Churn 100% Kernel**: Cập nhật `ghost_uptime_apply_stat` trong `kernel/ghost_uptime.c` tự động lùi `atime`, `mtime`, `ctime`, `btime` cho **toàn bộ thư mục (`S_ISDIR`)** trên F2FS/EXT4 (`/data/system`, `/data/user`, `/data/data/...`) về mốc 15 – 25 ngày trước theo Uptime ảo.
  - **Dumpsys Sanitizer 100% Kernel**: Bổ sung hàm `ghost_replace_string` vào `ghost_sanitize_batterystats_dump` (`kernel/ghost_uptime.c`), tự động thay thế in-place zero-delta trong luồng đọc `vfs_read()` của `dumpsys`: dập tắt `type=DEVICE_STARTUP`, `type=DEVICE_SHUTDOWN`, `due to SYSTEM_BOOT` $\rightarrow$ `due to USER_ACTION`, và DropBox boot records.
  - **Làm sạch hoàn toàn AnyKernel3**: Xóa bỏ hoàn toàn thư mục `payload/` và tập lệnh `ghost_reset.sh` khỏi AnyKernel3; sửa `anykernel.sh` chỉ nạp thuần túy `boot`, `vendor_boot` và `dtbo`.
  - **Biên dịch & Đóng gói Sạch 100%**: Biên dịch toàn bộ 6 gói xuất xưởng mới nhất, xác nhận gói Zip TWRP không chứa bất kỳ file payload hay script ngoài nào (`0 payload directory`).

### Vòng 8 (Triệt Tiêu Rò Rỉ Screen-On Time - Thuần 100% Kernel):
- **Hiện tượng rò rỉ**: Uptime hệ thống là 15 ngày 10 giờ, nhưng `dumpsys batterystats` chỉ in `Screen on: 1m 11s (45.7%) 1x`, `dumpsys usagestats` chỉ in `totalScreenOnTime=+13m`. Tỷ lệ SOT / Uptime chỉ đạt ~0.005%, tạo ra bất thường hành vi trầm trọng (Farm Box / Chạy ngầm không người dùng) khiến Shopee Risk Engine cắm cờ.
- **Giải pháp 100% Thuần Kernel trong `kernel/ghost_uptime.c`**:
  - Tự động tính toán thời gian sáng màn hình thực tế theo tỷ lệ người dùng thật: $\approx 18\%$ tổng thời gian Uptime ($\sim 4.3\text{ giờ/ngày}$, tương đương $\sim 2\text{ ngày } 18\text{ giờ}$ cho mốc 15 ngày).
  - Tự động tính số lần mở màn hình hợp lý: $\sim 42\text{ lần/ngày}$.
  - Can thiệp in-place zero-delta trên luồng đọc FIFO `vfs_read()` của `dumpsys`:
    - `Screen on: ` $\rightarrow$ `"Screen on: 2d 18h 36m (18%) 45x, Interactive: 2d 18h 36m (18%)   "`
    - `screen: duration:` $\rightarrow$ `"duration: 2d 18h 36m "`
    - `SEAMLESS ` $\rightarrow$ `"SEAMLESS 2d 18h 36m   "`
    - `totalScreenOnTime=+` $\rightarrow$ `totalScreenOnTime=+2d18h36m`
    - `totalElapsedTime=+` $\rightarrow$ `totalElapsedTime=+15d10h59m`

---

## 4. Danh Sách Gói Cài Đặt Xuất Xưởng Mới Nhất (Build Artifacts - Vòng 8 Thuần 100% Kernel)

Toàn bộ 6 gói đã được biên dịch thành công từ mã nguồn mới nhất và xuất ra thư mục gốc `G:\ssS21-test-RTC-main\`:

| Tên tệp gói cài đặt | Định dạng / Loại | Mã băm MD5 |
| :--- | :--- | :--- |
| **`G991B_ALL_KSUN_SUSFS_ODIN.tar.md5`** | ODIN Tar (KSU Next + SUSFS) | `816EA91F000E9004D17D68160539C62D` |
| **`G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar.md5`** | ODIN Tar LZ4 (KSU Next + SUSFS)| `8C7277CB422BFCEBCF6468AA6B8E94C8` |
| **`G991B_ALL_KSUN_SUSFS_TWRP.zip`** | AnyKernel3 TWRP (KSU Next + SUSFS)| `CEAFFBA4494B829E06DD2E6A910B6E43` |
| **`G991B_VANILLA_GHOST_UPTIME_ODIN.tar.md5`** | ODIN Tar (Stock boot/vendor) | `23F58253A0214BEC04645684D802C121` |
| **`G991B_VANILLA_GHOST_UPTIME_ODIN_LZ4.tar.md5`** | ODIN Tar LZ4 nén nhanh | `2FCD20D6D09C5C2EAB4310B86D669664` |
| **`G991B_VANILLA_GHOST_UPTIME_TWRP.zip`** | AnyKernel3 TWRP Flashable | `3AC46376548DB9B70C3CBFE3128D1503` |

---

## 5. Hướng Dẫn Nghiệm Thu 4 Điểm Rò Rỉ Sau Khi Nạp

Sau khi người dùng nạp bản flash mới vào máy, thực thi các lệnh sau qua ADB để kiểm tra kết quả:

1. **Kiểm tra Security Patch (Phải trả về `2024-05-01`)**:
   ```powershell
   adb shell getprop ro.build.version.security_patch
   adb shell getprop ro.vendor.build.security_patch
   ```

2. **Kiểm tra Data Churn mtime/btime (Phải lùi về tháng 8/2026, cách đây 23 ngày)**:
   ```powershell
   adb shell stat -c '%y' /data/system
   adb shell stat -c '%y' /data/user
   ```

3. **Kiểm tra Thông tin mạng Viettel & Dumpsys Telephony (Phải là VIETTEL và IN_SERVICE)**:
   ```powershell
   adb shell getprop gsm.operator.alpha
   adb shell getprop gsm.sim.operator.alpha
   adb shell getprop gsm.operator.numeric
   adb shell "dumpsys telephony.registry | grep -E 'mVoiceRegState|mDataRegState'"
   ```

4. **Kiểm tra Trạng thái Khóa Màn hình PIN & TrustAgent**:
   ```powershell
   adb shell dumpsys trust | grep -E 'deviceLocked|TrustAgent'
   ```

5. **Kiểm tra Đồng bộ Định danh Phần cứng & Uptime**:
   ```powershell
   adb shell "uname -a; getprop ro.serialno; getprop ro.boot.ap_serial; getprop ro.boot.em.did; getprop ro.boot.bore_cnt; cat /sys/devices/system/chip-id/unique_id; cat /proc/uptime"
   ```

6. **Quy tắc bất biến**: Tuyệt đối không dùng lệnh `git commit` hoặc `git push` trong suốt quá trình phát triển.
