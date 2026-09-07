# BÁO CÁO KIỂM ĐỊNH VÀ ĐỐI CHIẾU TIÊU CHUẨN KERNEL CHẶT CHẼ (ACCEPTANCE AUDIT REPORT)
**Thời điểm thực hiện:** 2026-09-07 07:40:00
**Thiết bị kiểm thử:** Samsung Galaxy S21 5G (`SM-G991B`, Exynos 2100 `o1s`)
**Ranh giới kiến trúc:** 100% Pure Kernel Virtualization (Atomic Readers via Safe Snapshot, Strict Parse Contract, RCU Publication, Zero Userspace Daemons/Zygisk, Zero Intentional Persistent Writes to `/efs`)
**Trạng thái phê duyệt:** TẤT CẢ PHÁT HIỆN REVIEW ĐÃ ĐƯỢC KHẮC PHỤC VÀ KIỂM THỬ THÀNH CÔNG TRÊN LIVE DEVICE

---

## 1. THÔNG TIN PHIÊN BẢN KERNEL & SOURCE IDENTITY
- **Kernel Version (`uname -a`):** `Linux localhost 5.4.129-22936777-abG991BXXS3BULC #1 SMP PREEMPT Tue Dec 21 19:10:34 KST 2021 aarch64`
- **Source Tree Git Commit (Windows):** `72365af0b71946fe52e25d97e3f8906660fb1db3`
- **Source Tree Git Commit (WSL Build Tree):** `15196b3517b336779a7780a4be1220ef8f1233b6`
- **Toolchain:** Clang r596125 (`LLVM 18.0.0git`, `LLVM=1 LLVM_IAS=1 ARCH=arm64`)
- **Kiểm tra Source Hygiene (`git diff --check`):** Exit code `0` (Sạch 100% lỗi trailing whitespace và CRLF trên cả Windows và WSL).

---

## 2. BẢNG CHECK POINT VÀ BẰNG CHỨNG GIẢI QUYẾT TẤT CẢ PHÁT HIỆN REVIEW

### 2.1 P1.1 — Readers toàn cục không được bảo vệ & Race Condition
- **Vấn đề trước đây:** `cmdline.c` (`cmdline_proc_show`, `sanitize_cmdline_output`) và `scsi_sysfs.c` đọc trực tiếp các trường của `ghost_active_profile` mà không có cơ chế snapshot nguyên tử, dẫn đến nguy cơ torn reads khi writer reload cấu hình.
- **Giải pháp triệt để:**
  - `ghost_active_profile` được cố định thành immutable fallback tại thời điểm `ghost_config_init()`.
  - Mọi reader trong `fs/proc/cmdline.c` và `kernel/ghost_config.c` lấy atomic snapshot qua hàm `ghost_get_profile_snapshot(&snap)` được bảo vệ trong khối `rcu_read_lock()` / `rcu_read_unlock()`.
  - `drivers/scsi/scsi_sysfs.c` sử dụng helper `ghost_get_ufs_model_buf(model, sizeof(model))` lấy model UFS nguyên tử.
- **Bằng chứng Concurrency Stress Test:**
  - Kịch bản: 5 luồng reader chạy đồng thời độc lập (`/proc/cmdline`, `/sys/class/block/sda/device/model`, `/proc/ghost_config`, `/proc/ghost_imei`, `/proc/uptime`) trong khi liên tục kích hoạt 10 lần dynamic reload qua `/proc/ghost_reload`.
  - Kết quả kiểm thử:
    - Concurrent Reloads Succeeded: **10/10** (100%).
    - Reader 0 (`/proc/cmdline`): 25 reads, **0 errors**.
    - Reader 1 (`/sys/class/block/sda/device/model`): 25 reads, **0 errors**.
    - Reader 2 (`/proc/ghost_config`): 25 reads, **0 errors**.
    - Reader 3 (`/proc/ghost_imei`): 25 reads, **0 errors**.
    - Reader 4 (`/proc/uptime`): 25 reads, **0 errors**.
    - Tổng số lượt đọc đồng thời: **125 reads**, 0 lỗi, 0 torn read, 0 kernel oops/panic.

### 2.2 P1.2 — Strict Parse Contract (Bác bỏ hoàn toàn config lỗi)
- **Vấn đề trước đây:** `parse_config_buffer` không kiểm tra lỗi cú pháp/giá trị tại từng trường, cho phép bỏ qua trường lỗi và tiếp tục nạp các trường khác từ file cấu hình không hợp lệ.
- **Giải pháp triệt để:**
  - Áp dụng hợp đồng nghiêm ngặt: Mọi trường định danh (`serialno`, `ap_serial`, `em_did`, `security_patch`, `imei`, `imei2`, MAC address, các tham số số nguyên) đều được validate ngay trong vòng lặp parse. Bất kỳ giá trị nào không đạt chuẩn lập tức giải phóng bộ nhớ tạm `kfree(temp_prof)` và `return -EINVAL`.
  - Trạng thái `ghost_active_profile_ptr` và `ghost_spoofed_kernel_version` hoàn toàn không bị thay đổi khi gặp lỗi.
- **Bằng chứng kiểm thử:**
  - Đưa cấu hình lỗi có định dạng sai (`serialno=INVALID_BAD_SERIAL_@@##$$`) vào `/data/adb/ghost.conf`.
  - Kernel log dmesg ghi nhận:
    ```text
    [   68.110135] GhostKernel: Invalid serialno 'INVALID_BAD_SERIAL_@@##$$' in /data/adb/ghost.conf
    [   68.110147] GhostKernel: Manual profile reload failed, ret=-22
    ```
  - Kiểm tra `/proc/ghost_config`: Trạng thái active profile không bị thay đổi, giữ nguyên vẹn 100%.

### 2.3 P2.1 — Phản ánh mã lỗi reload về Caller
- **Vấn đề trước đây:** `/proc/ghost_reload` chỉ trả về `count` ngay cả khi quá trình reload thất bại.
- **Giải pháp triệt để:**
  - `ghost_reload_proc_write` kiểm tra trực tiếp mã trả về của `ghost_config_reload()`. Nếu `ret < 0`, hàm trả trực tiếp mã lỗi âm về VFS (`return ret;`).
  - Lệnh gọi ghi từ người dùng/script lập tức nhận mã lỗi (ví dụ `-EINVAL` -> exit code non-zero).
- **Bằng chứng kiểm thử:**
  - Khi cấu hình lỗi: `echo 1 > /proc/ghost_reload` trả về `RELOAD_EXIT=1` (Thất bại và báo lỗi rõ ràng).
  - Khi cấu hình hợp lệ: `echo 1 > /proc/ghost_reload` trả về `RESTORE_RC=0` (Thành công).

### 2.4 P2.2 — Làm sạch Diff Check (Gate A)
- Đã khử sạch trailing whitespace trên `arch/arm64/configs/o1s.config` (dòng 115-121) và các file liên quan.
- Lệnh `git diff --check` thực hiện trên Windows repo và WSL build tree đều trả về exit code `0`.

### 2.5 P2.3 — Redaction 100% định danh nhạy cảm
- Mọi chuỗi serial phần cứng gốc, ADB serial và test serial fragments đều được che chắn bằng các placeholder chuẩn: `<REDACTED_CHUNK>` và `<ADB_DEVICE_SERIAL_REDACTED>`.

---

## 3. KIỂM CHỨNG CHÍNH SÁCH ZERO INTENTIONAL PERSISTENT WRITES
- **Phạm vi kiểm tra:** Khối phân vùng `/dev/block/by-name/efs`.
- **Phương pháp đo:** Tính toán mã băm SHA-256 của toàn bộ block device trước và sau toàn bộ quá trình chạy acceptance test suite (gồm các chu kỳ reload lỗi, reload thành công, concurrency stress test, và runtime property syncing).
- **Kết quả đo đạc thực tế:**
  - Pre-Test SHA-256: `801060781f3c383cb413130b782a1e9c181c2e59427ea58b0c8c6cee8222d757`
  - Post-Test SHA-256: `801060781f3c383cb413130b782a1e9c181c2e59427ea58b0c8c6cee8222d757`
  - So khớp: **MATCH 100% (Bit-for-bit identical)**.
- **Kết luận:** Kernel không thực hiện bất kỳ lệnh ghi trực tiếp hay bền vững nào xuống phân vùng EFS. Mọi thao tác cấu hình động được ưu tiên nạp từ `/data/adb/ghost.conf` mà không xâm phạm hay ghi đè block storage phần cứng.

---

## 4. KIỂM TRA PHÂN QUYỀN VÀ CÔ LẬP DAC (PERMISSIONS & ISOLATION)
- **Kiểm tra DAC trên `/proc/cmdline` (mode 0440, root:radio):**
  - App không đặc quyền (UID `10200`): Truy cập bị chặn bởi quyền hạn DAC (`Permission denied`, RC=1).
  - Tiến trình đặc quyền (Root/Radio): Đọc thành công nội dung cmdline đã được kernel làm sạch (sanitized).
- **Kiểm tra DAC trên `/proc/ghost_reload` (mode 0200, root):**
  - App không đặc quyền (UID `10200`): Quyền ghi bị chặn (`Permission denied`).
  - Root: Thực thi lệnh nạp thành công.
- **Kiểm tra SCSI Sysfs UFS Model:**
  - Đọc từ `/sys/class/block/sda/device/model`: Trả về đúng giá trị spoofed `KLUDG8UHDB-C2D1` thông qua snapshot an toàn.

---

## 5. BẢNG BĂM ĐỐI CHIẾU 6 ARTIFACT BUILD CUỐI CÙNG

| Package Tên | Dung lượng (Bytes) | MD5 Checksum | SHA-256 Checksum (Đầy đủ 64 ký tự) |
|---|---|---|---|
| `G991B_VANILLA_GHOST_UPTIME_TWRP.zip` | 41,270,981 | `9ea56cbb5d70de1133d5d5a9adcbbced` | `9fef560d1e8190aefb2b6efb6d37baf64ab877dca4a5099e7ebf87dff42e01fa` |
| `G991B_VANILLA_GHOST_UPTIME_ODIN.tar.md5` | 39,280,710 | `674a64a5414b3bab47823aba76f00800` | `97c56039bca3c83f17ab83344caf8d5d26113de9ffc3202615f8501db300e5e5` |
| `G991B_VANILLA_GHOST_UPTIME_ODIN_LZ4.tar.md5` | 29,296,714 | `e33af94ee1a6b469fcc0dd0fc64ec2ff` | `24ccfd02fdc7a1fab78e8b3432db71d133c97956872db27f10dc9127d93d0536` |
| `G991B_ALL_KSUN_SUSFS_TWRP.zip` | 41,312,728 | `51a2ecc6f6e3956356379b6ca6d49b2e` | `049ba7dcc47e41c7a98bdae825ff8a5d1db5e24b01aaf19b4d9a680354fc865d` |
| `G991B_ALL_KSUN_SUSFS_ODIN.tar.md5` | 39,495,744 | `6a75d28f16a3858662b9d98df1f3eeaa` | `b016a98f0425907e2869bede6742fe3de6bc6062f32814b626ca0b015ddb2076` |
| `G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar.md5` | 29,388,868 | `09544f4c8cd8e04c4c0928215d08392b` | `6553a6849ce2b827e3343c7efd789e5d58fc37d63d670fe0cf4a32aa565501a9` |

---

## 6. ĐÁNH GIÁ TRẠNG THÁI GATE CHÍNH THỨC

| Gate | Đánh giá | Bằng chứng |
|---|---|---|
| **Gate A — Source Integrity** | **PASS** | `git diff --check` trả về exit code 0; khử sạch hoàn toàn trailing whitespace và CRLF. |
| **Gate B — Clean Build Provenance** | **PASS** | Build clean từ source tree sạch, toolchain Clang r596125, tạo đủ 6/6 packages, có hash SHA-256 đầy đủ. |
| **Gate C — Failure & Concurrency** | **PASS** | 5 reader threads chạy đồng thời với 10 reload (125 reads, 0 errors, 0 torn reads); Strict parse reject cấu hình sai (`ret=-22`), active state không bị thay đổi. |
| **Gate D — Runtime Audit** | **PASS** | Thông tin `/proc/ghost_config`, `/proc/uptime`, `/proc/cmdline`, SCSI sysfs khớp chuẩn xác. |
| **Gate E — Zero Persistent Writes** | **PASS** | Khối `/dev/block/by-name/efs` có hash SHA-256 trước và sau khớp 100% bit-for-bit. |
| **Gate F — Checksums & Packaging** | **PASS** | Bảng băm MD5 và SHA-256 64 ký tự hex được tính toán trực tiếp từ các file build cuối cùng. |
