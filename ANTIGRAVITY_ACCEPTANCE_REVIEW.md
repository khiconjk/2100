# ANTIGRAVITY — KẾT QUẢ KIỂM TRA BÁO CÁO NGHIỆM THU

**Dự án:** Ghost Kernel — Samsung Galaxy S21 5G (`SM-G991B`, Exynos 2100 `o1s`)
**Tài liệu được kiểm tra:** `walkthrough.md`, `implementation_plan.md`, `GHOST_SYSTEM_AUDIT_LOG_FULL_POST_FLASH.md` và mã nguồn local tương ứng
**Mục tiêu:** 100% Pure Kernel cho pipeline spoofing/virtualization, không chủ động ghi bền vững xuống `/efs` hoặc `/data`
**Chế độ đánh giá:** Read-only; không sửa code, không sửa tài liệu, không flash thiết bị

## 1. Phán quyết nghiệm thu

**KẾT QUẢ: REJECT — CHƯA ĐẠT ĐỦ ĐIỀU KIỆN NGHIỆM THU 100%.**

Không được sử dụng các trạng thái sau trong báo cáo hiện tại:

- `RE-VALIDATION COMPLETE`;
- `ĐẠT 100%` cho RCU publication/lifetime;
- `Gate A đến Gate F đều PASS`;
- `toàn bộ blocker đã được giải quyết`.

Nhiều điểm của báo cáo mới đã được cải thiện và có bằng chứng runtime tốt hơn. Tuy nhiên, mã nguồn hiện tại vẫn chứa hai blocker concurrency nghiêm trọng. Bảng SHA-256 của cả sáu artifact cũng không khớp file thực tế. Ngoài ra, một số Gate vẫn thiếu bằng chứng có thể tái lập.

## 2. Blocker bắt buộc sửa trước khi build/flash lại

### P0 — Self-deadlock trong đường nạp cấu hình hợp lệ

Trong `kernel/ghost_config.c`, `ghost_config_reload()` khóa `ghost_config_mutex` trước khi gọi `ghost_read_file_and_parse()`:

```c
mutex_lock(&ghost_config_mutex);
ret = ghost_read_file_and_parse(GHOST_CONF_PATH_PRIMARY);
```

Khi file hợp lệ được parse thành công, `parse_config_buffer()` lại khóa chính `ghost_config_mutex` trước khi publish profile:

```c
mutex_lock(&ghost_config_mutex);
old_prof = rcu_dereference_protected(...);
rcu_assign_pointer(ghost_active_profile_ptr, temp_prof);
```

Linux mutex không phải mutex đệ quy. Cùng một task khóa lại mutex đang giữ có thể treo vĩnh viễn tại đường commit.

Log hiện tại báo:

```text
Source Status : [DEFAULT_FALLBACK]
```

Điều này giải thích tại sao đường commit cấu hình hợp lệ có thể chưa được chạy trong lần kiểm thử sau flash. Vì vậy, trạng thái fallback không phải bằng chứng rằng RCU reload đã hoạt động.

**Yêu cầu:** Chỉ có một tầng chịu trách nhiệm khóa writer. Sau khi sửa, phải kiểm thử ít nhất:

1. load thành công từ file cấu hình hợp lệ;
2. reload nhiều lần liên tiếp;
3. reload đồng thời với các reader;
4. file lỗi không làm treo task và không thay đổi active state;
5. timeout/watchdog xác nhận không có deadlock.

### P0 — RCU getter trả pointer sau khi đã nhả read lock

Các hàm sau lấy profile bằng `rcu_dereference()` nhưng gọi `rcu_read_unlock()` trước khi trả pointer hoặc pointer tới field:

- `ghost_get_profile()`;
- `ghost_get_active_serial()`;
- `ghost_get_active_ap_serial()`;
- `ghost_get_active_em_did()`.

Mẫu hiện tại:

```c
rcu_read_lock();
p = rcu_dereference(ghost_active_profile_ptr);
rcu_read_unlock();
return p->serialno;
```

Writer có thể publish profile mới, chạy `synchronize_rcu()` rồi `kfree(old_prof)` ngay sau khi getter nhả lock. Caller khi đó có thể truy cập con trỏ thuộc profile đã được giải phóng: nguy cơ use-after-free.

Các caller hiện có nằm trong những đường runtime quan trọng như:

- `drivers/usb/gadget/composite.c`;
- `fs/proc/cmdline.c`;
- `fs/read_write.c`;
- `ghost_sanitize_bootargs()`.

`ghost_get_active_serial_buf()` an toàn hơn vì copy dữ liệu trong khi còn giữ RCU read lock, nhưng các caller chưa được chuyển hết sang API dạng copy.

**Yêu cầu:** Không trả raw pointer ra ngoài RCU critical section. Dùng API copy-to-buffer/snapshot cho chuỗi và profile, hoặc yêu cầu caller giữ RCU lock trong toàn bộ thời gian sử dụng với contract rõ ràng. Sau đó chạy stress test reload/read.

## 3. Gate F thất bại — SHA-256 trong walkthrough không khớp

MD5 của sáu artifact khớp với bảng. Tuy nhiên, **SHA-256 của cả sáu artifact trong walkthrough đều sai**.

| Artifact | SHA-256 thực tế |
|---|---|
| `G991B_ALL_KSUN_SUSFS_ODIN.tar.md5` | `7625C1ED95309CAEEBEA8C14EA34FF9CB438CD252A58EB231E9184CF6B82A30B` |
| `G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar.md5` | `E3709187BEE7BB6E2C988251FD67A7B5C946389C6A4AB2971EB8051011D61C8D` |
| `G991B_ALL_KSUN_SUSFS_TWRP.zip` | `5C4D1DF42AFA5BCFAB0F2CE1BE1C61C1FD500DE65DC9B8E450C5738861BED52E` |
| `G991B_VANILLA_GHOST_UPTIME_ODIN.tar.md5` | `7C772191598D05FB6CB15C95585A3D0CB2259022C9BF0916A4A36D4F7D28560A` |
| `G991B_VANILLA_GHOST_UPTIME_ODIN_LZ4.tar.md5` | `E46CAD6DCDE5FC27A44ABA823D923FD4F9BA9ADD7B623C029F4F9BBE7B68C838` |
| `G991B_VANILLA_GHOST_UPTIME_TWRP.zip` | `BC7C55517FC8D5BDE6048BF7C1B8763602E8A97A7162E9C99CB4322C4F423071` |

**Yêu cầu:** Không chỉ sửa bảng hash cho bản hiện tại. Sau khi hai blocker P0 được sửa, phải clean-build, đóng gói lại và tính lại toàn bộ MD5/SHA-256 từ chính artifact cuối cùng. Ghi kèm source tree hash và toolchain identity.

## 4. Các yêu cầu chưa đủ bằng chứng

### 4.1 Profile vẫn chạy bằng fallback

Audit hiện báo `[DEFAULT_FALLBACK]`. Do đó chưa chứng minh được:

- load từ `/efs/ghost.conf`;
- successful two-phase commit;
- RCU profile replacement;
- giải phóng old profile;
- reload không deadlock;
- dữ liệu runtime thật sự đến từ external profile.

### 4.2 Vendor security patch chưa đạt mục tiêu hiển thị

Kết quả hiện tại:

- `ro.build.version.security_patch = 2024-05-01`;
- `ro.vendor.build.security_patch = 2021-12-01`.

Nếu mục tiêu là cả hai property hiển thị 2024-05-01 thì trạng thái chỉ là `PARTIAL`. Báo cáo phải tiếp tục ghi rõ đây là display-only override, không phải CVE backport.

### 4.3 Zero persistent write chưa được chứng minh thực nghiệm

Định nghĩa mới “không chủ động ghi bền vững xuống `/efs` hoặc `/data`” là hợp lý. Tuy nhiên, audit hiện chỉ khẳng định phân vùng EFS không đổi mà không kèm:

- hash block device trước/sau;
- block-write trace;
- sector/dm trace;
- bằng chứng filesystem journal tương ứng.

Source vẫn có `kernel_write()` và mở `O_RDWR/O_WRONLY` cho Android property area và USB configfs. Các write này có thể RAM-backed, nhưng vì vậy tài liệu chỉ được dùng cụm `zero intentional persistent writes to /efs or /data`, không được viết `không có write call` hoặc `source không chứa hàm ghi`.

### 4.4 Clean reproducible build chưa có evidence pack

Các artifact tồn tại không tự chứng minh clean reproducible build. Chưa thấy build log mới lưu cùng release để xác nhận:

- clean input tree;
- source revision/tree hash;
- WSL/toolchain version;
- full build command;
- exit code và warning set;
- hai lần build độc lập tạo cùng output hash.

### 4.5 Source integrity chưa hoàn toàn sạch

Kiểm tra `git diff --check` vẫn phát hiện trailing whitespace tại:

```text
arch/arm64/configs/o1s.config:115-121
```

Working tree cũng có phạm vi thay đổi rất lớn. Phải đóng băng chính xác source tree dùng để build và ghi lại tree hash trước khi nghiệm thu.

### 4.6 Log vẫn còn định danh thiết bị

Partial serial chunks và các bootargs chính đã được redact tốt hơn. Tuy nhiên, walkthrough vẫn in trực tiếp ADB device serial tại dòng mô tả reconnect.

**Yêu cầu:** Thay bằng `<ADB_DEVICE_SERIAL_REDACTED>`. Chạy lại redaction scan trên cả walkthrough và audit trước khi chia sẻ.

### 4.7 UFS/VPD vẫn chưa hoàn tất

Runtime hiện hiển thị `KLUDG8UHDB-C2D1`, nhưng điều đó chỉ chứng minh bề mặt runtime trả giá trị này. Nó không tự chứng minh đây là model vật lý thực tế của chip.

VPD 0x80/0x83 và WWID vẫn thiếu evidence độc lập có thể tái lập. Phase 3 phải giữ trạng thái `HOLD/RESEARCH`, không được tính là hoàn tất 100%.

## 5. Các kết quả đã có bằng chứng tốt hơn

Những nội dung sau có thể giữ lại trong báo cáo với phạm vi chính xác:

1. Partial-read serial đã được redact độc lập trong log mới.
2. Root concealment đã được kiểm thử với UID 2000 và App UID 10250.
3. Root đọc `/proc/cmdline` thành công; UID 2000 bị từ chối do permission policy.
4. Hai endpoint Device Tree được báo cáo cùng kích thước 2766 bytes và cùng MD5.
5. Uptime 23 ngày phù hợp với phép lấy phần nguyên từ khoảng 23.41 ngày.
6. `spoofed_kernel_version` đã được chuyển vào `temp_prof` trong parse path.
7. Orphan references tới `ghost_net`, `ghost_storage`, `ghost_thermal` đã sạch.
8. MD5 của sáu release artifact khớp với file local tại thời điểm review.

Các kết quả này không loại bỏ hai blocker concurrency và không đủ để nâng toàn bộ release lên `100% PASS`.

## 6. Trạng thái Gate chính xác

| Gate | Trạng thái hiện tại | Lý do |
|---|---|---|
| Gate A — Source & Tree Integrity | `PARTIAL` | Orphan references sạch, nhưng `git diff --check` còn lỗi và chưa có frozen tree hash. |
| Gate B — Clean Reproducible Builds | `UNPROVEN` | Thiếu clean-build logs và lần build độc lập tái tạo cùng hash. |
| Gate C — Failure Path & Validation | `FAIL` | Có self-deadlock và unsafe RCU pointer lifetime; valid config path chưa được test do fallback. |
| Gate D — Device Runtime Audit | `PARTIAL` | App UID, cmdline và DT đã tốt hơn; vendor patch và external profile load chưa đạt. |
| Gate E — Zero Persistent Writes | `UNPROVEN` | Chưa có partition hash trước/sau hoặc runtime block-write trace. |
| Gate F — Provenance & Packaging | `FAIL` | Cả sáu SHA-256 trong walkthrough không khớp artifact. |

## 7. Checklist bắt buộc trước lần nghiệm thu tiếp theo

- [ ] Loại bỏ nested locking/self-deadlock trong config reload/commit.
- [ ] Loại bỏ mọi getter trả pointer sau khi nhả RCU read lock.
- [ ] Chuyển caller chuỗi/profile sang API copy hoặc snapshot có lifetime an toàn.
- [ ] Clean-build và kiểm tra compiler/static-analysis warnings liên quan RCU/lifetime.
- [ ] Tạo file cấu hình hợp lệ và chứng minh `Source Status` không còn fallback.
- [ ] Reload profile nhiều lần liên tiếp không treo.
- [ ] Stress test đồng thời reader/reloader; không UAF, data race hoặc mixed profile.
- [ ] Test malformed config; active profile và kernel version không thay đổi.
- [ ] Kiểm tra lại build/vendor security patch theo expected value rõ ràng.
- [ ] Thu partition hash hoặc block-write trace trước và sau toàn bộ runtime audit.
- [ ] Sửa `git diff --check` đến khi sạch.
- [ ] Lưu clean-build evidence cho cả Vanilla và KSUN/SuSFS.
- [ ] Build và đóng gói lại sáu artifact từ source đã sửa.
- [ ] Tính lại MD5/SHA-256 và tự động đối chiếu bảng với file thực tế.
- [ ] Redact ADB device serial và chạy automated identifier scan.
- [ ] Giữ VPD/WWID ở trạng thái HOLD cho đến khi có evidence đầy đủ.

## 8. Quy tắc phê duyệt

Chỉ đổi trạng thái thành `APPROVED — 100% COMPLETE` khi đồng thời đáp ứng:

1. Hai blocker P0 đã được sửa và xác minh bằng test;
2. valid external profile load/reload hoạt động, không fallback và không deadlock;
3. không còn raw pointer vượt quá RCU lifetime;
4. Gate A–F đều có evidence tái lập và không còn `PARTIAL`, `FAIL`, `UNPROVEN`;
5. hash trong báo cáo khớp byte-for-byte với artifact cuối cùng;
6. báo cáo/log không chứa định danh thiết bị có thể khôi phục;
7. phạm vi “100% Pure Kernel” được định nghĩa rõ là pipeline nào và không mâu thuẫn với thành phần userspace đi kèm.

**Kết luận gửi Antigravity:** Kiến trúc có thể tiếp tục, nhưng bản hiện tại chưa đủ điều kiện nghiệm thu hoặc phát hành với nhãn 100%.
