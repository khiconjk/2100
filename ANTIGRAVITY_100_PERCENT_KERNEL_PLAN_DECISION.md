# QUYẾT ĐỊNH REVIEW PLAN 100% PURE KERNEL CHO ANTIGRAVITY

## Phạm vi quyết định

Tài liệu này review bản `implementation_plan.md` cập nhật lúc `2026-09-06 19:41:43` theo yêu cầu sản phẩm bắt buộc:

```text
Runtime implementation: 100% Linux kernel
No APK owner
No daemon owner
No Zygisk / LSPosed / Xposed implementation
No userspace module used to provide virtual identity
No Ghost-initiated write to EFS or /data
Device verification is read-only by default
```

Userspace scripts được phép tồn tại **chỉ như test harness trên máy phát triển**, không được là thành phần runtime cung cấp hoặc duy trì danh tính ảo.

## Quyết định cuối

```text
PURE_KERNEL_DIRECTION=APPROVED
CURRENT_PLAN_AS_WRITTEN=NOT_APPROVED
K1=CONDITIONALLY_APPROVED_AFTER_CORRECTIONS
K2=REJECTED_AS_DESIGNED
K3=RESEARCH_ONLY_NOT_APPROVED_FOR_IMPLEMENTATION
K4=RESEARCH_ONLY_NOT_APPROVED_FOR_IMPLEMENTATION
K5=REJECTED_BECAUSE_IT_INCLUDES_K2
RELEASE_OR_FLASH_AUTHORIZATION=NOT_GRANTED_BY_THIS_REVIEW
```

Lý do không duyệt toàn bộ plan hiện tại:

1. Plan bỏ mất Phase 0 và Gate A-F của phiên bản trước.
2. Working tree Windows vẫn có build blocker do consumer `ghost_net` mồ côi.
3. K-1 đề xuất thay đổi monotonic offset sau boot, có thể tạo bước nhảy thời gian kernel.
4. K-2 không thể nhận diện an toàn Telephony reply chỉ bằng `reply == 1` và quét 15 chữ số trong Binder Parcel.
5. K-1/K-4 xử lý VPD/HCI như chuỗi hoặc byte stream đơn giản, trong khi đây là giao thức nhị phân có cấu trúc và trạng thái.
6. Zero-disk proof được tuyên bố nhưng không có baseline/hash/log có thể kiểm chứng.
7. Các cam kết “100% không rò rỉ”, “100% giữ sóng” và mức rủi ro “thấp” không có bằng chứng tương ứng.

## Những phần đã đúng và nên giữ

Plan đã đáp ứng đúng mục tiêu 100% kernel ở các điểm sau:

- Không còn đề xuất Zygisk, LSPosed, APK hoặc daemon làm owner runtime.
- Tách `/proc/ghost_imei` khỏi Android Telephony authoritative path.
- Giữ Android Wi-Fi MAC randomization thay vì ép runtime MAC bằng profile.
- Nhận diện đúng ba surface UFS cơ bản đã PASS: vendor, model và revision.
- Nhận diện đúng hai file factory MAC đang tồn tại trên thiết bị:
  - `/mnt/vendor/efs/bluetooth/bt_addr`
  - `/mnt/vendor/efs/wifi/.mac.info`
- Hướng tới không ghi EFS hoặc `/data`.
- Nhận thức đúng K-3 có nguy cơ CP crash/mất sóng.

Kiểm tra source hiện tại không còn tìm thấy lời gọi `ghost_android_write_file()` trong `kernel/ghost_config.c`. Đây là tiến bộ đúng hướng, nhưng chưa đủ để chứng minh toàn bộ kernel không còn write path Ghost khác.

## BLOCKER 1 — Baseline Windows vẫn chưa buildable

Các file sau không tồn tại:

```text
kernel/ghost_net.c
kernel/ghost_storage.c
kernel/ghost_thermal.c
include/linux/ghost_net.h
```

Trong khi source Windows vẫn có consumer:

```text
net/core/net-sysfs.c: include <linux/ghost_net.h>
net/core/net-sysfs.c: ghost_net_ready / ghost_wifi_mac
net/core/dev.c: include <linux/ghost_net.h>
net/core/dev.c: ghost_net_ready / ghost_wifi_mac
```

WSL tree đã được kiểm tra và `kernel/Makefile` tại đó chỉ còn:

```make
obj-y += ghost_uptime.o ghost_config.o
```

Nhưng chỉ đồng bộ `kernel/Makefile` là chưa đủ. Header và symbol mồ côi trong `net/core` vẫn làm compile thất bại.

### Điều kiện bắt buộc

Khôi phục Phase 0 vào plan:

1. Inventory toàn bộ include, extern, object và call site của `ghost_net`, `ghost_storage`, `ghost_thermal`.
2. So sánh Windows và WSL cho từng consumer liên quan.
3. Chọn một baseline duy nhất làm nguồn build.
4. Loại bỏ toàn bộ stale reference hoặc khôi phục đầy đủ owner; không sửa một nửa.
5. Clean build Vanilla trước khi thêm chức năng mới.
6. Ghi source manifest/hash cho artifact.

Không được dùng artifact đã boot như bằng chứng rằng working tree Windows hiện tại buildable.

## BLOCKER 2 — K-1 Uptime Reconciliation không an toàn

Plan đề xuất sau khi `/efs/ghost.conf` được mount và parse:

```text
ghi uptime_days vào ghost_uptime_offset_ns
```

Đây là thiết kế không an toàn. Timekeeping khởi tạo rất sớm, còn EFS được mount muộn. Thay offset sau khi hệ thống đã chạy có thể làm các clock surface nhảy đột ngột.

Các subsystem có thể chịu ảnh hưởng:

- timer và delayed work;
- suspend/resume accounting;
- watchdog;
- process start time;
- Binder timeout;
- scheduler/runtime accounting;
- Android service duration và alarm;
- filesystem timestamp comparison.

Source hiện tại còn ghi rõ các hook timekeeping trung tâm đã bị vô hiệu hóa “for system stability”. Vì vậy không được đánh giá thay offset muộn là rủi ro thấp.

### Quyết định kiến trúc bắt buộc

Giữ thiết kế session offset hiện tại:

```text
offset được chọn đúng một lần tại early boot
offset bất biến trong toàn bộ boot session
RTC phần cứng không thay đổi
realtime=off
```

Khi profile được tải muộn, `/proc/ghost_config` phải phản ánh offset runtime đã chọn. Không được làm ngược lại bằng cách thay clock đang chạy cho khớp một giá trị cấu hình tải muộn.

Nếu người dùng muốn `uptime_days` từ config là authoritative, nó phải được cung cấp trước timekeeping init bằng một cơ chế early-boot thích hợp và trở thành một quyết định sản phẩm khác. Không được đọc EFS muộn rồi đổi monotonic offset.

### Kết luận cho mục uptime K-1

```text
CURRENT_DESIGN=REJECTED
SAFE_DIRECTION=REPORT_RUNTIME_OFFSET_IN_PROFILE
```

## BLOCKER 3 — K-2 Binder Telephony không khả thi như mô tả

Plan giả định có thể:

1. Chọn mọi Binder reply bằng `reply == 1`.
2. Tìm chuỗi 15 chữ số trong buffer.
3. Thay bằng IMEI profile.

Giả định này sai hoặc thiếu các điều kiện nền tảng.

### Binder reply không tự mang đủ danh tính method

Request có transaction code/interface context, nhưng reply không đơn giản chứa lại đầy đủ tên interface. Muốn liên kết reply với `IPhoneSubInfo` cần trạng thái giao dịch đáng tin cậy, process/target/method mapping và lifecycle chính xác.

`reply == 1` chỉ nói đây là reply, không chứng minh reply thuộc Telephony hay chứa IMEI.

### Parcel string không phải ASCII tùy ý

Java/AIDL thường biểu diễn string trong Parcel theo layout có length, alignment và UTF-16. Quét 15 byte ASCII liên tiếp có thể:

- không tìm thấy IMEI;
- match nhầm dữ liệu số khác;
- sửa sai offset/alignment;
- làm hỏng Parcel;
- ảnh hưởng một Binder service không liên quan.

### Một reply có thể chứa nhiều field

Không thể kết luận mọi chuỗi 15 chữ số là IMEI. Nó có thể là subscriber identifier, timestamp, ICCID fragment hoặc dữ liệu ứng dụng khác.

### Rủi ro hệ thống

`drivers/android/binder.c` là đường IPC cốt lõi của toàn Android. Lỗi tại đây có thể gây:

- system_server crash;
- phone process crash;
- Binder transaction corruption;
- deadlock hoặc use-after-free;
- bootloop;
- hỏng IPC không liên quan đến Telephony.

Do đó mức rủi ro “thấp” là sai. Mức đúng phải là **cao đến rất cao**.

### Quyết định cho K-2

```text
K2_IMPLEMENTATION=REJECTED
K2_RESEARCH=ALLOWED_READ_ONLY
```

Chỉ được nghiên cứu read-only transaction ownership, Parcel encoding và method mapping. Không được patch `binder_transaction()` dựa trên heuristic “reply + 15 digits”.

Vì K-5 bao gồm K-2, K-5 cũng không được duyệt.

## BLOCKER 4 — K-1 VPD implementation chưa đúng cấu trúc source

Plan nêu các hàm:

```text
show_vpd_vpd_pg80()
show_vpd_vpd_pg83()
```

Các symbol này không tồn tại. Source dùng macro tạo hàm theo mẫu:

```text
show_vpd_##_page
```

Tên thực tế cần được xác định sau preprocessing/source inspection, không được đoán trong plan.

### VPD 0x80 và 0x83 là binary protocol

Không được coi VPD là chuỗi text thông thường:

- Có page code và page length.
- Page 0x83 chứa một hoặc nhiều identification descriptor.
- Descriptor có code set, association, identifier type và length.
- Sysfs binary read phải tôn trọng `off` và `count`.
- WWID có thể được derive từ page 0x83.

Thay tùy ý một chuỗi hoặc mask toàn page có thể tạo descriptor không hợp lệ hoặc làm WWID không nhất quán.

### Điều kiện để duyệt phần UFS mở rộng

1. Parse và document layout byte thực tế của pg80/pg83 trên thiết bị.
2. Xác định owner duy nhất cho WWID và từng VPD page.
3. Tạo output có cấu trúc hợp lệ và giữ nguyên length semantics.
4. Hỗ trợ binary partial read với `off/count`.
5. Bảo đảm WWID, pg83 và profile cùng một identifier.
6. Test storage boot/mount/I/O sau thay đổi.

Cho đến khi các điều kiện này có trong plan:

```text
UFS_BASIC_SYSFS=APPROVED_EXISTING
UFS_WWID_VPD=NOT_APPROVED
```

## BLOCKER 5 — MAC EFS paths tồn tại nhưng root cause chưa proven

Thiết bị xác nhận hai file MAC tồn tại và có kích thước 17 byte. Điều này không chứng minh:

- Bluetooth HAL thực sự dùng đúng file đó trong build hiện tại;
- `macloader` đọc file tại thời điểm nào;
- giá trị có bị cache không;
- firmware/driver có ghi đè sau đó không;
- profile đã sẵn sàng trước lần đọc đầu tiên không.

### Timing problem

Nếu HAL/daemon đọc factory MAC trước khi `/efs/ghost.conf` được parse, VFS hook có thể trả default/ephemeral profile. Khi profile tải xong, HAL đã cache địa chỉ và không đọc lại.

### Điều kiện duyệt MAC K-1

1. Thu evidence read-only xác định process và thời điểm mở hai file.
2. Xác định profile readiness trước lần đọc.
3. Thiết kế partial read/offset/EOF đúng.
4. Không ghi file EFS.
5. Giữ Android per-network randomized MAC làm runtime authority.
6. Kiểm tra reboot và Bluetooth/Wi-Fi reconnect nhiều lần.

Phần này được duyệt ở mức **investigation**, chưa được duyệt implementation.

## BLOCKER 6 — K-3 thiếu protocol specification

Plan giả định Samsung Shannon IPC chứa trực tiếp phản hồi tương ứng `RIL_REQUEST_GET_IMEI` và có thể thay 15 ký tự.

Chưa có evidence về:

- channel thực tế;
- message ID;
- framing;
- endianness;
- fragmentation;
- checksum;
- encryption/encoding;
- multi-SIM correlation;
- request/reply lifecycle.

Không được patch `ipc_io_device.c` hoặc `shm_ipc.c` chỉ bằng tìm chuỗi 15 chữ số.

```text
K3=RESEARCH_ONLY
```

Mọi nghiên cứu phải read-only. Không được inject packet vào modem và không được gây CP reset.

## BLOCKER 7 — K-4 chọn hook quá rộng và UART chưa được chứng minh

### SCSI

`scsi_execute_req()` không đồng nghĩa chỉ với userspace `SG_IO`. Nó có thể được kernel dùng cho internal discovery và inquiry. Hook rộng tại đây có thể làm chính kernel nhận identity giả và ảnh hưởng probing.

Trước khi chọn hook phải trace chính xác đường SG_IO trên kernel này và phân biệt:

- userspace pass-through;
- kernel internal command;
- boot-time discovery;
- sysfs cached VPD.

### Bluetooth UART

Plan chưa chứng minh Bluetooth controller của o1s dùng `/dev/ttySAC1` cho HCI response cần xử lý. Ngay cả khi đúng, serial driver nhận byte stream có thể bị fragment; nó không tự biết ranh giới HCI event nếu không có parser/state machine phù hợp.

Hook tại `samsung_tty.c` có blast radius lớn và có thể ảnh hưởng thiết bị UART khác.

```text
K4=RESEARCH_ONLY
```

## BLOCKER 8 — Zero-Disk proof hiện không hợp lệ

Plan tuyên bố raw block đã chứng minh EFS gốc còn nguyên, nhưng không cung cấp:

- pre-flash baseline hash;
- post-flash hash;
- timestamp;
- resolved block device mapping;
- mount state;
- audit log;
- phạm vi block/inode được so sánh.

Lệnh:

```text
strings /dev/block/sda1 | grep R5
```

không chứng minh zero-disk:

- Chuỗi tìm được có thể là dữ liệu stale/deleted hoặc bản sao khác.
- Không chứng minh file hiện tại giữ nguyên.
- Không chứng minh không có block nào bị ghi.
- Có thể xuất dữ liệu định danh nhạy cảm.
- `/dev/block/sda1` không nên hardcode nếu chưa resolve `by-name/efs`.

EFS cũng có thể thay đổi hợp lệ bởi modem/hệ thống trong quá trình boot, nên hash toàn partition có thể đổi mà Ghost không phải nguyên nhân.

### Định nghĩa zero-disk có thể kiểm chứng

Plan chỉ được tuyên bố:

```text
No Ghost code path intentionally performs write operations to EFS or /data.
```

Để chứng minh, cần:

1. Static inventory mọi `kernel_write`, `vfs_write`, file open `O_WRONLY/O_RDWR`, truncate/unlink/rename do Ghost gọi.
2. Runtime instrumentation read-only hoặc trace cho Ghost write attempts.
3. Baseline hợp lệ được thu trước thử nghiệm.
4. Không xuất raw identifier trong log/report.

Không được dùng cụm “chip flash nguyên bản 100%” nếu không có backup factory trước mọi bản Ghost từng chạy.

## BLOCKER 9 — Root Stealth xuất hiện trong tiêu đề nhưng không có plan

Tiêu đề nói K-DIV và Root Stealth, nhưng năm lựa chọn chủ yếu xử lý identity surfaces. Không có threat model, owner, scope hoặc verification đầy đủ cho root stealth.

Hai lệnh `which su` và `stat /system/bin/su` không đủ chứng minh root stealth. Thiết bị audit hiện có `/system/bin/su` và shell có thể gọi `su`.

Yêu cầu:

- Loại “Root Stealth” khỏi tiêu đề/core acceptance; hoặc
- Tạo một plan riêng, không gộp với K-DIV.

Không được để root stealth trở thành tiêu chí ngầm làm mở rộng scope của K-1/K-2.

## BLOCKER 10 — Cam kết và xếp hạng rủi ro không trung thực

Các câu sau phải bị loại khỏi plan:

```text
triệt tiêu 100% rò rỉ vật lý
bảo toàn sóng 100%
bảo toàn 100% chức năng phần cứng
không một công cụ nào đọc được
K-2 risk thấp
K-5 risk thấp
```

Không hệ thống test hiện tại nào chứng minh các cam kết tuyệt đối đó.

Đánh giá rủi ro phù hợp hơn:

| Option | Risk hợp lý |
| --- | --- |
| K-1 basic sysfs/VFS presentation | Medium |
| K-1 VPD/WWID structured virtualization | Medium-High |
| K-1 late monotonic mutation | Critical / Reject |
| K-2 Binder reply mutation | High-Critical / Reject as designed |
| K-3 CPIF mutation | Critical |
| K-4 SG_IO/HCI bus mutation | High |
| K-5 | Critical because it includes K-2 |

## Dữ liệu nhạy cảm phải được redacted

Plan hiện ghi trực tiếp serial, AP serial, WWID, VPD serial và MAC thật. Không cần các giá trị này để mô tả kiến trúc.

Thay bằng:

```text
<FACTORY_SERIAL_REDACTED>
<FACTORY_AP_SERIAL_REDACTED>
<FACTORY_UFS_WWID_REDACTED>
<FACTORY_UFS_SERIAL_REDACTED>
<FACTORY_WIFI_MAC_REDACTED>
<FACTORY_BT_MAC_REDACTED>
```

Không commit hoặc chia sẻ plan chứa raw identifiers.

## Plan 100% kernel được phép sau khi sửa

Antigravity phải viết lại plan theo các phase dưới đây.

### Phase 0 — Baseline Recovery

- Chọn một source tree authority duy nhất.
- Loại mọi stale include/symbol/object.
- Clean build Vanilla.
- Ghi source manifest và build log.
- Không thêm feature.

### Phase 1 — Zero-Write and Profile Safety

- Static inventory tất cả write path của Ghost.
- Parse vào profile tạm.
- Validate toàn bộ field và cross-field invariant.
- Publish bằng cơ chế reader-safe thực sự; writer mutex đơn lẻ không đủ cho lockless readers.
- Không sử dụng struct lớn tùy ý trên kernel stack.

### Phase 2 — Existing Surface Correctness

- Serial VFS phải đúng partial read, offset và EOF.
- Hook phải quyết định trước hay sau underlying read; không double-advance `*pos`.
- Battery có một owner duy nhất.
- UFS basic vendor/model/rev có một owner duy nhất.
- Không thay clock offset sau early boot.

### Phase 3 — K-1 Evidence and Narrow Extensions

- Trace read-only factory MAC consumers/timing.
- Document pg80/pg83 binary layout.
- Chỉ đề xuất từng extension nhỏ sau khi root cause/evidence hoàn chỉnh.
- Không gộp MAC, VPD và uptime vào cùng một patch chưa kiểm chứng.

### Phase 4 — Verification Gates

Khôi phục đầy đủ:

```text
Gate A: source integrity
Gate B: clean reproducible builds
Gate C: failure path and partial-read tests
Gate D: read-only device runtime audit
Gate E: no-Ghost-write evidence with honest limits
Gate F: artifact provenance and hashes
```

### Phase 5 — Packaging

- Chỉ package sau khi Gate A-F PASS.
- Tách Vanilla và KSU/SuSFS.
- MD5 chỉ dùng khi format Odin yêu cầu.
- SHA-256 dùng cho artifact integrity.
- Không flash tự động; flash cần yêu cầu riêng của người dùng.

## Approval Matrix chi tiết

| Feature | Decision | Điều kiện |
| --- | --- | --- |
| Remove stale Ghost build references | APPROVE | Phải xử lý cả consumer, không chỉ Makefile |
| Remove Ghost disk write paths | APPROVE | Static và runtime evidence |
| Session-only RAM profile fallback | APPROVE | Không ghi seed, document identity churn |
| Atomic/staged profile validation | APPROVE WITH CORRECTION | Phải bảo vệ lockless readers |
| Existing serial VFS presentation | APPROVE WITH CORRECTION | Đúng pos/partial-read/EOF |
| Battery mask at driver owner | APPROVE | Không thêm duplicate VFS owner |
| UFS vendor/model/rev sysfs | APPROVE EXISTING | Không gọi là full UFS identity |
| UFS WWID/VPD mutation | HOLD | Cần binary layout và tests |
| Factory MAC VFS interception | HOLD | Cần trace consumer/timing |
| Preserve Android MAC randomization | APPROVE | Không ép runtime MAC profile |
| Change uptime offset after config load | REJECT |
| Report actual runtime offset in profile | APPROVE |
| Binder heuristic IMEI rewrite | REJECT |
| CPIF packet rewrite | RESEARCH ONLY |
| SG_IO/HCI low-level rewrite | RESEARCH ONLY |
| K-5 combination | REJECT |
| Display-only security patch | OUT OF CORE SCOPE | Không phải security update |
| Root stealth | SEPARATE PLAN REQUIRED |

## Chỉ thị cuối cho Antigravity

Không triển khai K-2, K-3, K-4 hoặc K-5 từ plan hiện tại.

Được phép viết lại plan theo mục tiêu 100% kernel và bắt đầu bằng Phase 0. Sau khi Phase 0 chứng minh clean reproducible build, chỉ K-1 phạm vi hẹp mới được xem xét từng phần:

```text
1. baseline correctness
2. zero-write proof with honest limits
3. reader-safe profile publication
4. serial/battery/UFS basic surface correctness
5. runtime profile reports the immutable early-boot uptime offset
6. evidence collection for MAC and VPD before implementation
```

Mỗi proposed change phải ghi:

```text
Observed evidence
Root cause: proven / hypothesis / unknown
Exact kernel owner
Initialization timing
Concurrency model
Binary/text format semantics
Smallest patch boundary
Failure modes
Static/build tests
Read-only runtime tests
Stop condition
Artifact/source provenance
```

Plan chỉ được phê duyệt thực thi sau khi không còn blocker và không còn cam kết tuyệt đối thiếu bằng chứng.
