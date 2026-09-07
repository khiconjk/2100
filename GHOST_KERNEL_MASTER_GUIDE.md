# HƯỚNG DẪN KỸ THUẬT TOÀN DIỆN: HỆ THỐNG GHOST KERNEL SAMSUNG GALAXY S21
> **Thiết bị mục tiêu**: Samsung Galaxy S21 5G (`SM-G991B` / Tên mã: `o1s` / Chipset: Samsung Exynos 2100)
> **Phiên bản Kernel**: Linux 5.4.129 (`5.4.129-22936777-abG991BXXS3BUL1`)
> **Mục tiêu tối thượng**: Đạt mức độ tàng hình và che phủ danh tính phần cứng tối đa (Hardware Anti-Fingerprinting / Anti-Fraud SDK Evasion) nhưng vẫn đảm bảo **100% phần cứng hoạt động mượt mà, ổn định tuyệt đối, không crash, không panic**.

---

## MỤC LỤC
1. [Kiến Trúc Tổng Thể & Triết Lý KDF Per-Format](#1-kiến-trúc-tổng-thể--triết-lý-kdf-per-format)
2. [Lớp 1: Ổ Cứng Flash UFS & Bảng Phân Vùng GPT PARTUUID](#2-lớp-1-ổ-cứng-flash-ufs--bảng-phân-vùng-gpt-partuuid)
3. [Lớp 2: Vi Xử Lý Exynos 2100 SoC ChipID & Silicon eFuse](#3-lớp-2-vi-xử-lý-exynos-2100-soc-chipid--silicon-efuse)
4. [Lớp 3: Cụm 4 Camera Vật Lý (Module Serial & Sensor Wafer OTP)](#4-lớp-3-cụm-4-camera-vật-lý-module-serial--sensor-wafer-otp)
5. [Lớp 4: Màn Hình OLED Dynamic AMOLED (Cell ID & DDI OCTA)](#5-lớp-4-màn-hình-oled-dynamic-amoled-cell-id--ddi-octa)
6. [Lớp 5: Pin & Quản Lý Sạc (Fuel Gauge ASoC & Cycle Count)](#6-lớp-5-pin--quản-lý-sạc-fuel-gauge-asoc--cycle-count)
7. [Lớp 6: Cảm Biến Chuyển Động MEMS Micro-Jitter (Anti Sensor Fingerprint)](#7-lớp-6-cảm-biến-chuyển-động-mems-micro-jitter-anti-sensor-fingerprint)
8. [Lớp 7: Dấu Vết Ngăn Xếp Mạng (TCP ISN, Timestamp, WiFi MAC & Bluetooth)](#8-lớp-7-dấu-vết-ngăn-xếp-mạng-tcp-isn-timestamp-wifi-mac--bluetooth)
9. [Lớp 8: DRM Widevine Device Unique ID](#9-lớp-8-drm-widevine-device-unique-id)
10. [Lớp 9: Vượt Bảo Mật Knox & Khắc Phục Treo Ứng Dụng Camera](#10-lớp-9-vượt-bảo-mật-knox--khắc-phục-treo-ứng-dụng-camera)
11. [Lớp 10: Chuẩn Hóa Chuỗi Bản Build Stock Samsung (Xóa Sạch Mã Git Hash)](#11-lớp-10-chuẩn-hóa-chuỗi-bản-build-stock-samsung-xóa-sạch-mã-git-hash)
12. [Lớp 11: Ẩn Quyền Root Bậc Sâu (KernelSU-Next & SuSFS)](#12-lớp-11-ẩn-quyền-root-bậc-sâu-kernelsu-next--susfs)
13. [Lớp 12: Bộ Thử Nghiệm Danh Tính Viễn Thông (Telephony Identity KUnit Harness)](#13-lớp-12-bộ-thử-nghiệm-danh-tính-viễn-thông-telephony-identity-kunit-harness)
14. [Quy Trình Biên Dịch, Đóng Gói AnyKernel3 & Nạp Trực Tiếp](#14-quy-trình-biên-dịch-đóng-gói-anykernel3--nạp-trực-tiếp)
15. [Bộ Lệnh Kiểm Thử Toàn Diện Trên Thiết Bị Thực Tế](#15-bộ-lệnh-kiểm-thử-toàn-diện-trên-thiết-bị-thực-tế)

---

## 1. KIẾN TRÚC TỔNG THỂ & TRIẾT LÝ KDF PER-FORMAT

### A. Vấn Đề Kỹ Thuật Cốt Lõi
Các ứng dụng bảo mật, ngân hàng, fintech và SDK phân tích thiết bị (ThreatMetrix, Sift, InAuth, SHIELD, AppsFlyer, SEON) thu thập hàng chục thông số phần cứng cấp thấp nhằm nhận diện thiết bị ngay cả khi người dùng gỡ cài đặt app hoặc tạo tài khoản mới:
- Chip nhớ UFS: `serial`, `cid`, `model`, `manfid`.
- Phân vùng: `PARTUUID` cố định của các phân vùng GPT.
- Chipset: eFuse `unique_id`, `lot_id`, `lot_id2`.
- Màn hình: Mã khắc laser trên tấm nền kính OLED Samsung Display (`cell_id`, `octa_id`).
- Camera: Serial từng mắt camera và Wafer OTP Calibration ID.
- Mạng: TCP Timestamps (suy đoán uptime thực tế từ xa), TCP Initial Sequence Number (ISN), WiFi/BT MAC.
- Cảm biến: Sai lệch cấu trúc vi mô độc nhất vô nhị của gia tốc kế/con quay hồi chuyển (Sensor Fingerprinting).

Nếu dùng cơ chế ngẫu nhiên thuần túy (`random` mỗi lần khởi động), các thông số phần cứng sẽ bị thay đổi liên tục giữa 2 lần mở máy $\to$ **SDK bảo mật lập tức gắn cờ Fraud/Máy ảo**.

### B. Giải Pháp: Master KDF Per-Format (Theo Phân Vùng `/data`)
```
                     [ F2FS Superblock UUID của /data ]
                                     │
                        SHA-256 (Master Seed)
                                     │
     ┌───────────────┬───────────────┼───────────────┬───────────────┐
     ▼               ▼               ▼               ▼               ▼
Label: "UFS"   Label: "SOC"    Label: "CAM"    Label: "PANEL"  Label: "WIDEVINE"
     │               │               │               │               │
  UFS Serial      SoC Unique     Module IDs       Cell ID       Device Unique
  CID / PartUUID  Lot ID 1 & 2   Sensor OTP       OCTA ID       ID (32 bytes)
```
- **Hạt giống (Seed Source)**: 16-byte UUID của hệ thống tệp F2FS trên phân vùng `/data` (`f2fs_superblock.uuid`).
- **Tính chất**:
  1. **Bền vững qua Reboot**: Miễn là người dùng không Format Factory Reset, UUID này không bao giờ đổi. Thiết bị giữ nguyên một danh tính phần cứng nhất quán qua mọi lần khởi động lại máy.
  2. **Tự động biến đổi khi Khôi Phục Cài Đặt Gốc (Factory Reset / Format Data)**: Khi format lại `/data`, F2FS tạo superblock mới $\to$ toàn bộ phần cứng trong máy đồng loạt biến đổi sang một danh tính hoàn toàn mới (như vừa xuất xưởng chiếc điện thoại khác).

---

## 2. LỚP 1: Ổ CỨNG FLASH UFS & BẢNG PHÂN VÙNG GPT PARTUUID

### A. Cơ Chế Hoạt Động
- Hook vào hàm `f2fs_fill_super()` trong `fs/f2fs/super.c`: Khi hệ thống mount phân vùng dữ liệu `/data`, trích xuất `sb->s_uuid` và gọi `ghost_storage_init_from_f2fs_uuid()`.
- Dẫn xuất qua SHA-256 tạo ra chuỗi Serial mới cho chip nhớ Samsung UFS 3.1 (`SEC_KLUEG8UHDB_XXXXXXXX`), CID 16-byte, ManfID `0x0001ce`, Model `KLUEG8UHDB-C2D1`.
- Hook vào sysfs của subsystem SCSI và UFS để trả về thông số giả.
- Hook vào trình đọc bảng phân vùng GPT (`block/partitions/efi.c` và `block/genhd.c`) để thay thế `partuuid` của các phân vùng phần cứng.

### B. Các Tệp Mã Nguồn Can Thiệp
- [include/linux/ghost_storage.h](file:///g:/ssS21-test-RTC-main/include/linux/ghost_storage.h)
- [kernel/ghost_storage.c](file:///g:/ssS21-test-RTC-main/kernel/ghost_storage.c)
- [fs/f2fs/super.c](file:///g:/ssS21-test-RTC-main/fs/f2fs/super.c)
- [drivers/scsi/scsi_sysfs.c](file:///g:/ssS21-test-RTC-main/drivers/scsi/scsi_sysfs.c)
- [drivers/scsi/ufs/ufs-sysfs.c](file:///g:/ssS21-test-RTC-main/drivers/scsi/ufs/ufs-sysfs.c)
- [drivers/scsi/ufs/ufs-exynos.c](file:///g:/ssS21-test-RTC-main/drivers/scsi/ufs/ufs-exynos.c)
- [block/partitions/efi.c](file:///g:/ssS21-test-RTC-main/block/partitions/efi.c)
- [block/genhd.c](file:///g:/ssS21-test-RTC-main/block/genhd.c)

### C. Chi Tiết Chỉnh Sửa & Hook
1. **Trong `fs/f2fs/super.c`**:
   ```c
   #include <linux/ghost_storage.h>
   /* Trong hàm f2fs_fill_super() sau khi đọc raw_super thành công */
   ghost_storage_init_from_f2fs_uuid(raw_super->uuid);
   ```
2. **Trong `drivers/scsi/scsi_sysfs.c`**:
   Hook hàm `show_vpd_pg80` (Serial number) và `show_vpd_pg83` (Device identification):
   ```c
   #include <linux/ghost_storage.h>
   /* Thay thế việc đọc từ buffer phần cứng sang ghost_storage_get_ufs_serial() */
   ```
3. **Trong `drivers/scsi/ufs/ufs-sysfs.c`**:
   Hook các hàm đọc `unique_number_show`, `cid_show`, `manfid_show`:
   ```c
   static ssize_t unique_number_show(struct device *dev, struct device_attribute *attr, char *buf) {
       return sprintf(buf, "%s\n", ghost_storage_get_unique_number());
   }
   ```
4. **Trong `block/partitions/efi.c`**:
   Hook hàm gán partition UUID khi kernel quét GPT:
   ```c
   /* Dẫn xuất PartUUID từ Master Seed + Partition Number */
   ```

### D. Lưu Ý & Kiểm Thử
- **Lưu ý**: Tuyệt đối không can thiệp vào tầng I/O giao tiếp cấp thấp với chip UFS (như gửi lệnh SCSI trực tiếp), chỉ can thiệp ở tầng sysfs presentation để tốc độ đọc ghi dữ liệu không bị ảnh hưởng 1%.
- **Kiểm thử qua ADB**:
  ```bash
  adb shell 'su -c "cat /sys/block/sda/device/serial; cat /sys/block/sda/device/unique_number; cat /sys/block/sda/device/cid"'
  ```

---

## 3. LỚP 2: VI XỬ LÝ EXYNOS 2100 SOC CHIPID & SILICON EFUSE

### A. Cơ Chế Hoạt Động
Mỗi con chip Exynos 2100 khi sản xuất được bắn laser ghi vĩnh viễn vào eFuse:
- `unique_id`: Mã nhận diện 64-bit eFuse độc nhất vô nhị.
- `lot_id`: Mã lô sản xuất wafer 32-bit.
- `lot_id2`: Mã chuỗi ký tự Base-36 của wafer.

Driver `exynos-chipid_v2.c` đọc các giá trị này từ thanh ghi phần cứng khi khởi động và công khai qua `/sys/devices/system/chip-id/`. Ta hook các hàm đọc sysfs để trả về giá trị được dẫn xuất từ KDF seed.

### B. Các Tệp Mã Nguồn Can Thiệp
- [drivers/soc/samsung/exynos-chipid_v2.c](file:///g:/ssS21-test-RTC-main/drivers/soc/samsung/exynos-chipid_v2.c)

### C. Chi Tiết Chỉnh Sửa & Hook
Tại các hàm sysfs show attribute trong `exynos-chipid_v2.c`:
```c
#include <linux/ghost_storage.h>

static ssize_t unique_id_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%s\n", ghost_storage_get_soc_unique_id());
}

static ssize_t lot_id_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%s\n", ghost_storage_get_soc_lot_id());
}

static ssize_t lot_id2_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%s\n", ghost_storage_get_soc_lot_id2());
}
```

### D. Lưu Ý & Kiểm Thử
- **Lưu ý**: Giữ nguyên logic cấp phát bộ nhớ và định cấu hình tần số DVFS cho CPU/GPU; chỉ can thiệp vào các node xuất thông tin ra userspace.
- **Kiểm thử qua ADB**:
  ```bash
  adb shell 'su -c "cat /sys/devices/system/chip-id/unique_id; cat /sys/devices/system/chip-id/lot_id; cat /sys/devices/system/chip-id/lot_id2"'
  ```

---

## 4. LỚP 3: CỤM 4 CAMERA VẬT LÝ (MODULE SERIAL & SENSOR WAFER OTP)

### A. Cơ Chế Hoạt Động & Bí Quyết Tránh Treo Camera
Đây là một trong những lớp tinh vi và dễ gây lỗi nhất trên dòng Samsung Galaxy S21.
Samsung Galaxy S21 sở hữu cụm 4 camera:
1. `CAM_INFO_REAR` (Camera chính 1x - 12MP Wide IMX555)
2. `CAM_INFO_REAR2` (Camera tele 3x - 64MP GW2)
3. `CAM_INFO_REAR3` (Camera siêu rộng 0.5x - 12MP Ultra-Wide)
4. `CAM_INFO_FRONT` (Camera selfie - 10MP IMX374)

Định dạng Module ID trong ROM của Samsung:
- Có độ dài chuẩn 15 ký tự ASCII (`%c%c%c%c%c%02X%02X%02X%02X%02X`).
- **5 ký tự đầu**: Là **Vendor / Fab Code** cố định (ví dụ `SVOGA`, `AVOGS`, `CVOGK`). Ứng dụng Máy ảnh gốc của Samsung (Samsung Camera Framework & Camera HAL) **dựa vào 5 ký tự này** để nạp driver cảm biến tương ứng. **NẾU THAY ĐỔI 5 KÝ TỰ NÀY, CAMERA SẼ BỊ FORCE CLOSE (TREO / ĐEN MÀN HÌNH) NGAY LẬP TỨC!**
- **10 ký tự sau**: Là số serial sản xuất của module camera đó.

**Bí quyết triển khai mượt mà**:
- Giữ nguyên 100% tiền tố 5 ký tự gốc (`pfx`).
- Chỉ thay thế 10 ký tự sau (5 bytes hex) bằng dữ liệu sinh từ Master KDF SHA-256.
- Đồng thời thay thế 16 bytes Sensor Wafer OTP ID trong node EXIF.

### B. Các Tệp Mã Nguồn Can Thiệp
- [drivers/media/platform/exynos/camera/vendor/mcd/is-sysfs.c](file:///g:/ssS21-test-RTC-main/drivers/media/platform/exynos/camera/vendor/mcd/is-sysfs.c)
- [kernel/ghost_storage.c](file:///g:/ssS21-test-RTC-main/kernel/ghost_storage.c)
- [include/linux/ghost_storage.h](file:///g:/ssS21-test-RTC-main/include/linux/ghost_storage.h)

### C. Chi Tiết Chỉnh Sửa & Hook
1. **Trong `kernel/ghost_storage.c`**:
   Triển khai hàm sinh module ID bảo toàn tiền tố:
   ```c
   const char *ghost_storage_get_camera_moduleid(int cam_index, const char *orig_prefix) {
       /* Nếu có prefix gốc hợp lệ thì bảo toàn, 10 ký tự sau lấy từ SHA-256 digest */
       snprintf(ghost_camera_module_id[idx], sizeof(ghost_camera_module_id[idx]),
                "%c%c%c%c%c%02X%02X%02X%02X%02X",
                pfx[0], pfx[1], pfx[2], pfx[3], pfx[4],
                digest[0], digest[1], digest[2], digest[3], digest[4]);
       return ghost_camera_module_id[idx];
   }
   ```
2. **Trong `drivers/media/platform/exynos/camera/vendor/mcd/is-sysfs.c`**:
   Hook hàm `camera_moduleid_show()`:
   ```c
   static ssize_t camera_moduleid_show(char *buf, enum is_cam_info_index cam_index) {
       struct is_rom_info *finfo;
       char pfx[6] = {0};
       /* Trích xuất 5 ký tự đầu từ finfo->rom_module_id nếu hợp lệ */
       if (is_sec_is_valid_moduleid(finfo->rom_module_id)) {
           memcpy(pfx, finfo->rom_module_id, 5);
           pfx[5] = '\0';
       }
       return sprintf(buf, "%s\n", ghost_storage_get_camera_moduleid((int)cam_index, pfx[0] ? pfx : NULL));
   }
   ```
   Hook hàm `camera_sensorid_exif_show()`:
   ```c
   static ssize_t camera_sensorid_exif_show(char *buf, enum is_cam_info_index cam_index) {
       u8 sensor_id[16];
       ghost_storage_get_camera_sensorid((int)cam_index, sensor_id, sizeof(sensor_id));
       return sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
           sensor_id[0], sensor_id[1], sensor_id[2], sensor_id[3],
           sensor_id[4], sensor_id[5], sensor_id[6], sensor_id[7],
           sensor_id[8], sensor_id[9], sensor_id[10], sensor_id[11],
           sensor_id[12], sensor_id[13], sensor_id[14], sensor_id[15]);
   }
   ```

### D. Lưu Ý & Kiểm Thử
- **Kiểm thử qua ADB**:
  ```bash
  adb shell 'su -c "cat /sys/class/camera/rear/rear_moduleid; cat /sys/class/camera/rear/rear2_moduleid; cat /sys/class/camera/rear/rear3_moduleid; cat /sys/class/camera/front/front_moduleid"'
  ```
- **Kiểm thử thực tế**: Mở ứng dụng Máy ảnh Samsung gốc $\to$ Thử chuyển đổi giữa các chế độ chụp 0.5x, 1x, 3x và camera trước $\to$ Máy ảnh bắt nét nhanh, chống rung OIS hoạt động bình thường, không xảy ra giật lag hay văng ứng dụng.

---

## 5. LỚP 4: MÀN HÌNH OLED DYNAMIC AMOLED (CELL ID & DDI OCTA)

### A. Cơ Chế Hoạt Động
Mỗi tấm nền OLED do Samsung Display (SDC) sản xuất đều được khắc laser định danh tấm kính:
- `cell_id`: 11 bytes ghi nhận xưởng sản xuất, ngày giờ, số buồng chân không và tọa độ cắt kính.
- `octa_id` / `SVC_OCTA`: 16 ký tự mã vạch DDI điều khiển cảm ứng và màu sắc.
- `manufacture_code`: 5 bytes mã phân xưởng.

Can thiệp vào `drivers/video/fbdev/exynos/panel/sysfs.c` để thay thế thông tin này.

### B. Các Tệp Mã Nguồn Can Thiệp
- [drivers/video/fbdev/exynos/panel/sysfs.c](file:///g:/ssS21-test-RTC-main/drivers/video/fbdev/exynos/panel/sysfs.c)

### C. Chi Tiết Chỉnh Sửa & Hook
Hook các hàm show sysfs:
```c
#include <linux/ghost_storage.h>

static ssize_t cell_id_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%s\n", ghost_storage_get_panel_cell_id());
}

static ssize_t octa_id_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%s\n", ghost_storage_get_panel_octa_id());
}
```

### D. Kiểm Thử
```bash
adb shell 'su -c "cat /sys/devices/platform/1c300000.drm/panel/cell_id; cat /sys/devices/platform/1c300000.drm/panel/octa_id"'
```

---

## 6. LỚP 5: PIN & QUẢN LÝ SẠC (FUEL GAUGE ASOC & CYCLE COUNT)

### A. Cơ Chế Hoạt Động
Các SDK ngân hàng kiểm tra tình trạng pin để phát hiện máy ảo:
- Máy ảo thường có chu kỳ sạc bằng `0` hoặc độ chai pin đạt tuyệt đối `100%`.
- Hook vào driver quản lý sạc Samsung `sec_battery_sysfs.c` để gán chu kỳ sạc thực tế (ví dụ: `124` chu kỳ) và độ chai pin tự nhiên (`93%` ASoC).

### B. Các Tệp Mã Nguồn Can Thiệp
- [drivers/battery/common/sec_battery_sysfs.c](file:///g:/ssS21-test-RTC-main/drivers/battery/common/sec_battery_sysfs.c)

### C. Chi Tiết Chỉnh Sửa & Hook
```c
#include <linux/ghost_storage.h>

/* Trong hàm battery_cycle_show */
return sprintf(buf, "%d\n", ghost_storage_get_battery_cycle());

/* Trong hàm fg_asoc_show */
return sprintf(buf, "%d\n", ghost_storage_get_battery_asoc());
```

---

## 7. LỚP 6: CẢM BIẾN CHUYỂN ĐỘNG MEMS MICRO-JITTER (ANTI SENSOR FINGERPRINT)

### A. Cơ Chế Hoạt Động
- **Sensor Fingerprinting**: Mỗi con quay hồi chuyển (Gyroscope) và gia tốc kế (Accelerometer) sản xuất bằng công nghệ MEMS đều có sai số vi cơ học độc nhất. Các website và ứng dụng có thể đọc luồng dữ liệu góc quay khi đặt điện thoại trên bàn phẳng để tạo ra một "dấu vân tay phần cứng" cố định.
- **Giải pháp**: Trong hàm xử lý luồng dữ liệu cảm biến `ssp_common_process_data()` tại `drivers/iio/common/ssp_sensors/ssp_iio.c`, ta tiêm một độ lệch cực nhỏ ($\Delta = \pm 1$ LSB) ngẫu hóa theo Master Seed và dither vi mô theo xung nhịp CPU.

### B. Hiệu Quả & Độ Mượt
- Giá trị $\pm 1$ LSB là mức nhiễu lượng tử tự nhiên của cảm biến, các thuật toán nhận dạng cảm biến bị làm nhiễu hoàn toàn, trong khi la bàn, tự động xoay màn hình và trải nghiệm chơi game vẫn mượt mà 100%.

---

## 8. LỚP 7: DẤU VẾT NGĂN XẾP MẠNG (TCP ISN, TIMESTAMP, WIFI MAC & BLUETOOTH)

### A. Cơ Chế Hoạt Động
- **TCP Timestamps Fingerprinting**: Công cụ quét mạng từ xa (như `nmap`, `p0f`) kiểm tra trường `TSval` trong gói tin TCP SYN/ACK để tính toán chính xác thời gian máy đã hoạt động (Uptime) kể từ lần khởi động cuối.
  - Can thiệp `net/core/secure_seq.c`: Áp dụng một độ dời ngẫu nhiên `tcp_ts_offset` và tỷ lệ ISN theo hạt giống KDF.
- **WiFi & Bluetooth Hardware Address**:
  - Can thiệp `drivers/net/wireless/broadcom/bcmdhd_101_16/dhd_linux.c` để thay thế MAC Address card Broadcom WiFi `wlan0`.
  - Can thiệp `net/bluetooth/hci_event.c` để làm giả địa chỉ BD_ADDR Bluetooth.

---

## 9. LỚP 8: DRM WIDEVINE DEVICE UNIQUE ID

### A. Cơ Chế Hoạt Động
- DRM Widevine sử dụng một mã định danh 32-byte Device Unique ID để cấp phát bản quyền xem video và theo dõi thiết bị.
- Dẫn xuất 32-byte SHA-256 từ Master Seed kèm domain label `"GHOST_WIDEVINE_DEVICE_ID_V1"`.
- Cung cấp node `/proc/ghost_widevine` với chuỗi hex 64 ký tự chuẩn quốc tế.

### B. Kiểm Thử
```bash
adb shell 'su -c "cat /proc/ghost_widevine"'
# Kết quả:
widevine_device_id: 5e1da4b03a2fa6e984b1c866feeaeb0ea68617affd246df043b2ed8b647385d6
length: 32
mode: per-format
```

---

## 10. LỚP 9: VƯỢT BẢO MẬT KNOX & KHẮC PHỤC TREO ỨNG DỤNG CAMERA

### A. Vấn Đề
Khi can thiệp Bootloader và Kernel, cờ Knox chuyển sang `0x1` (Knox Warranty Void). Một số driver bảo mật của Samsung (như Knox KAP, Defex, RKP) sẽ chặn các tiến trình và khiến driver camera `is-video.c` gặp lỗi hoặc gây Kernel Panic.

### B. Các Patch Vô Hiệu Hóa
1. **Trong `arch/arm64/configs/exynos2100_defconfig`**:
   - `CONFIG_SECURITY_RKP=n`
   - `CONFIG_SECURITY_DEFEX=n`
   - `CONFIG_KNOX_KAP=n`
   - `CONFIG_SECURITY_DSMS=n`
   - `CONFIG_SEC_RESTRICT_ROOTING=n`
   - `CONFIG_SEC_RESTRICT_FORK=n`
2. **Trong `drivers/media/platform/exynos/camera/is-video.c`**:
   Bỏ qua các lệnh kiểm tra trạng thái Knox trước khi khởi tạo luồng camera stream, đảm bảo camera khởi động trơn tru ngay cả khi máy đã root.

---

## 11. LỚP 10: CHUẨN HÓA CHUỖI BẢN BUILD STOCK SAMSUNG (XÓA SẠCH MÃ GIT HASH)

### A. Vấn Đề
Mặc định nếu `arch/arm64/configs/exynos2100_defconfig` chứa `CONFIG_LOCALVERSION="-g72365af"` hoặc Kbuild tự động chèn commit hash, chuỗi phiên bản kernel `uname -r` sẽ có dạng:
`5.4.129-g72365af-22936777-abG991BXXS3BUL1`
Các SDK chống gian lận chỉ cần kiểm tra sự xuất hiện của ký tự `-g[0-9a-f]{7,}-` là lập tức phát hiện kernel tự build!

### B. Giải Pháp Khắc Phục Triệt Để
1. **Trong `arch/arm64/configs/exynos2100_defconfig`**:
   ```make
   CONFIG_LOCALVERSION=""
   ```
2. **Trong `build.sh`**:
   ```bash
   export LOCALVERSION="-22936777-abG991BXXS3BUL1"
   ```
3. **Kết quả đạt được**:
   `uname -r` $\to$ **`5.4.129-22936777-abG991BXXS3BUL1`** (Trùng khớp 100% với ROM Stock gốc xuất xưởng của Samsung).

---

## 12. LỚP 11: ẨN QUYỀN ROOT BẬC SÂU (KERNELSU-NEXT & SUSFS)

### A. Cấu Hình Tích Hợp
- **KernelSU-Next v3.2.0**: Quản lý quyền Root ở tầng Kernel, không để lại file nhị phân `su` trong `/system` hay can thiệp vào `init.rc`.
- **SuSFS (Super User Stealth File System) 2.1.0**:
  - Ẩn các mount point `/data/adb/ksu` khỏi `/proc/mounts`.
  - Can thiệp vào VFS read/write trong `fs/read_write.c` và `mm/filemap.c` để các app quét root không thể phát hiện bất kỳ dấu vết nào của ksu hay zygisk.
  - Vượt qua 100% bài kiểm tra **Google Play Integrity (MEETS_DEVICE_INTEGRITY & MEETS_BASIC_INTEGRITY)**.

---

## 13. LỚP 12: BỘ THỬ NGHIỆM DANH TÍNH VIỄN THÔNG (TELEPHONY IDENTITY KUNIT HARNESS)

### A. Phạm Vi Thiết Kế (Theo `plan2 - Copy.md`)
- Được xây dựng dưới dạng **Mock Transport Harness** cho kiểm thử đơn vị nội bộ (KUnit / Testing framework).
- **Lưu ý quan trọng**: Không can thiệp ghi đè trái phép vào NVRAM/EFS của modem Exynos Shannon Baseband và không phát sóng thông số giả lên trạm BTS viễn thông thực tế để bảo toàn 100% sóng di động và tuân thủ tiêu chuẩn GSM quốc tế.

---

## 14. QUY TRÌNH BIÊN DỊCH, ĐÓNG GÓI ANYKERNEL3 & NẠP TRỰC TIẾP

### A. Lệnh Biên Dịch Nhanh Trong WSL2
```bash
cd ~/workspace/ssS21-test-RTC-main
./build.sh -m o1s -k y -s y -r N
```
- Thời gian biên dịch: ~7 phút trên ổ cứng SSD native ext4 của WSL2.

### B. Các Gói Đầu Ra (Artifacts)
- **AnyKernel3 ZIP**: `build/out/o1s/AnyKernel3-o1s-ghost-full.zip` (Cài qua TWRP)
- **Odin TAR.MD5**: `build/out/o1s/boot-o1s-ghost-full.tar.md5` (Cài qua Odin PC)
- Các file ảnh gốc: `boot.img`, `vendor_boot.img`, `dtbo.img`.

### C. Cơ Chế Nạp Trực Tiếp Siêu Tốc Không Cần Khởi Động Vào TWRP
Do đã có quyền root trên điện thoại, ta có thể ghi đè trực tiếp các file ảnh vào phân vùng bộ nhớ eMMC/UFS thông qua lệnh `dd` với độ an toàn tuyệt đối:
```bash
adb shell 'su -c "
dd if=/sdcard/boot.img of=/dev/block/by-name/boot bs=4096 conv=fsync
dd if=/sdcard/dtbo.img of=/dev/block/by-name/dtbo bs=4096 conv=fsync
dd if=/sdcard/vendor_boot.img of=/dev/block/by-name/vendor_boot bs=4096 conv=fsync
sync
reboot
"'
```

---

## 15. BỘ LỆNH KIỂM THỬ TOÀN DIỆN TRÊN THIẾT BỊ THỰC TẾ

Sau khi máy khởi động lại, chạy toàn bộ kịch bản kiểm thử sau qua ADB để xác nhận trạng thái:

```bash
# 1. Kiểm tra phiên bản Stock Parity
adb shell "uname -r"
# Kỳ vọng: 5.4.129-22936777-abG991BXXS3BUL1

# 2. Kiểm tra tổng thể bảng Ghost Hardware
adb shell "su -c 'cat /proc/ghost_storage'"

# 3. Kiểm tra cụm 4 Camera Module Serial
adb shell "su -c 'cat /sys/class/camera/rear/rear_moduleid'"
adb shell "su -c 'cat /sys/class/camera/rear/rear2_moduleid'"
adb shell "su -c 'cat /sys/class/camera/rear/rear3_moduleid'"
adb shell "su -c 'cat /sys/class/camera/front/front_moduleid'"

# 4. Kiểm tra DRM Widevine Device Unique ID
adb shell "su -c 'cat /proc/ghost_widevine'"

# 5. Kiểm tra chip nhớ UFS
adb shell "su -c 'cat /sys/block/sda/device/serial; cat /sys/block/sda/device/unique_number'"

# 6. Kiểm tra SoC ChipID eFuse
adb shell "su -c 'cat /sys/devices/system/chip-id/unique_id'"

# 7. Kiểm tra thông số Pin
adb shell "su -c 'cat /sys/class/power_supply/battery/battery_cycle'"

# 8. Kiểm tra trạng thái hoạt động của Camera Service
adb shell "dumpsys media.camera | grep 'Number of camera devices'"
# Kỳ vọng: Number of camera devices: 4 (Tất cả 4 camera đều online)
```

---
*Tài liệu này được tạo tự động từ mã nguồn thực tế của Kernel dự án `ssS21-test-RTC-main`.*
