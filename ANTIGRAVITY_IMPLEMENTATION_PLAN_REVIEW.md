# Review yêu cầu chỉnh sửa Implementation Plan cho Antigravity

## Mục đích tài liệu

Tài liệu này là phản hồi kỹ thuật cho kế hoạch `implementation_plan.md` về K-DIV. Hãy dùng nó để **viết lại kế hoạch trước khi sửa code**.

Không được hiểu tài liệu này là yêu cầu triển khai ngay. Trước hết phải giải quyết trạng thái source, xác minh nguyên nhân lỗi trên thiết bị và làm cho build có thể tái lập.

Các ràng buộc hiện tại:

- Không `git commit`.
- Không `git push`.
- Không flash, wipe, reset, format hoặc thay đổi thiết bị nếu chưa có yêu cầu rõ ràng.
- Không sửa `/efs/FactoryApp/serial_no` hay dữ liệu hiệu chuẩn EFS.
- Mọi kiểm tra thiết bị mặc định phải là read-only.
- Không tuyên bố security patch mới nếu source kernel chưa thực sự chứa các bản vá tương ứng.

## Kết luận review

Implementation plan hiện tại **chưa được chấp thuận để triển khai** vì không khớp với working tree và có các mâu thuẫn kiến trúc quan trọng.

Các lỗi chính:

1. Nhiều thay đổi được đề xuất đã tồn tại trong source.
2. Một số file mà plan yêu cầu sửa hiện đã bị xóa.
3. Working tree hiện có build blocker.
4. Mục tiêu “Zero-Disk Modification” mâu thuẫn với code đang ghi vào `/data` và `/efs`.
5. Nguyên nhân lỗi nạp profile chưa được chứng minh.
6. VFS interception chưa có semantics đúng cho partial read, offset và EOF.
7. UFS, MAC, Bluetooth và IMEI mới chỉ bao phủ một phần các surface.
8. Verification plan chưa đủ để chứng minh correctness, zero-disk hoặc regression safety.

## 1. Trạng thái source bắt buộc phải hiểu trước

Working tree hiện không phải một baseline sạch:

- 431 tracked files thay đổi.
- 1.930 dòng thêm và 15.132 dòng xóa.
- 348 tracked files bị xóa.
- 83 tracked files bị sửa.
- Không có thay đổi staged.
- KernelSU-Next là detached HEAD và có nhiều file sửa nội bộ.
- Có hàng trăm module và artifact untracked.

### Build blocker hiện hữu

`kernel/Makefile` vẫn chứa:

```make
obj-y += ghost_uptime.o ghost_net.o ghost_storage.o ghost_thermal.o ghost_config.o
```

Nhưng các file sau không còn tồn tại trong working tree:

```text
kernel/ghost_net.c
kernel/ghost_storage.c
kernel/ghost_thermal.c
include/linux/ghost_net.h
include/linux/ghost_storage.h
include/linux/ghost_thermal.h
```

Trong khi đó, một số consumer vẫn include hoặc sử dụng symbol từ các header/file đã mất, ví dụ:

```text
net/core/net-sysfs.c
net/core/dev.c
kernel/Makefile
```

Vì vậy không được bắt đầu Component 4 hoặc tuyên bố build thành công từ working tree hiện tại. Trước tiên phải xác định rõ:

- Khôi phục các component bị xóa; hoặc
- Loại bỏ toàn bộ reference/consumer liên quan; hoặc
- Xác định một baseline/commit khác mới là source thực sự tạo artifact.

Không được tự ý chọn một trong ba phương án nếu chưa có bằng chứng về ý định của chủ dự án.

## 2. Component 1 đang đề xuất lại code đã tồn tại

Plan cho rằng `ghost_read_file_and_parse()` vẫn dùng `filp_open()` trực tiếp và cần đổi sang root của PID 1.

Source hiện tại tại `kernel/ghost_config.c` đã thực hiện:

```c
if (rel_path && ghost_get_init_root(&init_root) == 0) {
    filp = file_open_root(init_root.dentry, init_root.mnt,
                          rel_path, O_RDONLY, 0);
    path_put(&init_root);
}
```

Sau đó mới fallback sang `filp_open()`.

Vì thiết bị vẫn báo `[DEFAULT_FALLBACK]`, giả thuyết “chỉ do worker dùng sai namespace” chưa được chứng minh. Viết lại cùng cơ chế sẽ không chắc sửa được lỗi.

### Yêu cầu sửa plan

Thay Component 1 bằng một pha chẩn đoán read-only, phải phân biệt ít nhất:

- `/efs` chưa mount.
- `/efs/ghost.conf` không tồn tại.
- `file_open_root()` trả lỗi.
- fallback `filp_open()` trả lỗi.
- file đọc được nhưng rỗng.
- parser thất bại.
- profile tải thành công nhưng bị default profile/serial guard ghi đè sau đó.
- worker hết số lần retry trước khi filesystem sẵn sàng.

Plan phải yêu cầu lưu errno/trạng thái từng pha vào endpoint audit hoặc kernel log. Chỉ thiết kế bản sửa sau khi có log chứng minh nguyên nhân.

## 3. “Zero-Disk Modification” hiện không đúng

`ghost_apply_serial_guard()` đang gọi các đường ghi sau:

```text
data/.ghost_seed
data/system/.ghost_seed
efs/ghost_serial.txt
efs/FactoryApp/serial_no
mnt/vendor/efs/FactoryApp/serial_no
data/.ghost_ap_seed
data/system/.ghost_ap_seed
efs/ghost_ap_seed.txt
```

Đặc biệt, code ghi trực tiếp vào `efs/FactoryApp/serial_no`, trái hoàn toàn với ràng buộc “không biến đổi dữ liệu phân vùng gốc”.

### Yêu cầu sửa plan

Nếu zero-disk là yêu cầu bất biến, plan phải:

1. Liệt kê và vô hiệu hóa mọi write path tới `/efs` và `/data` thuộc cơ chế virtualization.
2. Không dùng seed file trên disk để quảng cáo là “RAM-only”.
3. Mô tả rõ seed tồn tại trong một boot session hay phải persistent qua reboot.
4. Nếu cần persistence thì phải thừa nhận không còn zero-disk và xin quyết định sản phẩm mới.
5. Không dùng chính VFS interceptor để xác minh dữ liệu gốc, vì interceptor sẽ che kết quả kiểm tra.

Một lệnh `cat /efs/FactoryApp/serial_no` sau interceptor chỉ chứng minh output đã bị thay, không chứng minh dữ liệu thật trên EFS không bị ghi.

## 4. Component 2 bị trùng và có lỗi semantics

Source hiện tại đã intercept `serial_no` và `ghost_serial.txt` trong `vfs_read()`. Battery `batt_type` cũng đã được mask trực tiếp trong `sec_battery_sysfs.c`.

Không được thêm lớp interception thứ hai nếu chưa quyết định một single owner cho mỗi surface.

### Lỗi phải giải quyết trong thiết kế VFS

Plan hiện chưa xử lý:

- `*pos` và partial reads.
- EOF sau khi đã đọc hết dữ liệu ảo.
- Buffer nhỏ hơn serial cộng newline.
- Nhiều lần đọc liên tiếp.
- `pread` và `read_iter`.
- Lỗi `copy_to_user()`.
- Match nhầm file cùng tên ở path khác.
- Profile reload đồng thời với reader.
- Semantics của `ret` so với số byte thật đã copy.
- Overhead của `kmalloc()` và scan trong đường nóng `vfs_read()`.

Code hiện tại có thể trả lại cùng 11 byte serial ở các offset khác nhau và không kiểm tra kết quả `copy_to_user()`. Nếu thêm newline thành 12 byte mà không thiết kế partial-read semantics, bug càng rõ hơn.

### Battery

`batt_type` hiện được mask tại nơi tạo sysfs output. Plan phải chọn một trong hai:

- Mask tại driver sysfs show; hoặc
- Mask tại một lớp interception tổng quát.

Không làm cả hai. Nếu mã pin được sinh từ seed thì phải ổn định trong phạm vi đã định; không random mới trên mỗi lần đọc.

## 5. Component 3 chưa bao phủ UFS/SCSI đầy đủ

Source hiện đã thay:

- vendor thành `SAMSUNG`.
- model thành profile/default Samsung model.
- revision thành `0100`.

Nhưng diff hiện tại lại bỏ các filter cũ cho:

- VPD page 0x80.
- WWID.
- serial.
- CID.
- manufacturer ID.

Thông tin thật còn có thể xuất hiện qua inquiry/VPD, UFS driver attributes, debugfs hoặc các API khác. Vì vậy không được viết “đảm bảo tất cả đường dẫn đều hiển thị Samsung” nếu verification chỉ `cat` ba file sysfs.

### Yêu cầu sửa plan

Lập inventory cụ thể cho từng surface UFS/SCSI. Với mỗi surface phải ghi:

```text
Surface | Owner function | Data source | Visibility | Intended behavior | Test
```

Không hardcode một model UFS trước khi kiểm tra model đó phù hợp với dung lượng, revision và đặc tính thiết bị mục tiêu. Một tổ hợp vendor/model/capacity không tồn tại cũng tạo inconsistency.

## 6. Component 4 hiện không khả thi và không đầy đủ

### Wi-Fi MAC

`address_show()` chỉ thay output sysfs. MAC vẫn có thể được đọc từ underlying `net_device`, rtnetlink/ioctl, driver/HAL và frame mạng.

Logic hiện tại còn chỉ match tên bắt đầu bằng `wlan`, không bao phủ `p2p0` như plan tuyên bố.

Plan phải ngừng dùng cụm từ “ảo hóa MAC hoàn toàn” nếu chỉ sửa sysfs. Trước tiên cần xác định phạm vi rõ ràng: chỉ output sysfs hay underlying network identity.

### Bluetooth

Audit finding nói có Wi-Fi và Bluetooth MAC, profile có `bt_mac`, nhưng Proposed Changes không có bất kỳ owner/hook/test nào cho Bluetooth. Đây là một phần bị bỏ sót hoàn toàn.

### IMEI

`/proc/ghost_imei` không phải nguồn dữ liệu mà Android Telephony sử dụng. `iphonesubinfo` và ứng dụng lấy dữ liệu qua framework/Binder/RIL/modem.

Test `/proc/ghost_imei` có thể PASS trong khi `iphonesubinfo` vẫn trống. Do đó plan hiện kiểm tra sai surface so với finding ban đầu.

Ngoài ra `kernel/ghost_net.c` và header liên quan đang bị xóa, nên không được ghi `[MODIFY] kernel/ghost_net.c` mà không giải quyết baseline trước.

## 7. Component 5 không phải bản vá bảo mật thật

Thay chuỗi `ro.build.version.security_patch` thành `2024-05-01` không backport bất kỳ CVE fix nào. Không được mô tả kết quả này như thiết bị đã được vá bảo mật.

Patcher hiện tại:

- Quét buffer property có kích thước cố định.
- Match một số chuỗi ngày khá rộng.
- Ghi lại toàn bộ buffer property.
- Không chứng minh property serial/checksum/cache giữa các process vẫn đúng.
- Không kiểm tra short read/short write đầy đủ.
- Không chứng minh các process đã mmap property area sẽ thấy thay đổi nhất quán.

### Yêu cầu sửa plan

Khuyến nghị loại bỏ Component 5 khỏi mục tiêu compatibility/virtualization. Nếu vẫn giữ cho môi trường lab, phải đổi tên thành “display-only property override”, ghi cảnh báo rõ rằng firmware vẫn ở security level thật, và không được dùng nó làm bằng chứng an toàn.

## 8. Profile reload thiếu atomicity và validation

Parser hiện sửa trực tiếp global `ghost_active_profile` từng field rồi đánh dấu `is_loaded=true`. Readers không dùng cùng mutex/RCU khi đọc profile.

Các rủi ro:

- File cấu hình hợp lệ một phần tạo profile lai cũ/mới.
- Serial hoặc IMEI sai format vẫn được lưu.
- MAC parse nhận số vượt `0xff` rồi truncate.
- `kstrtouint()` error bị bỏ qua.
- Reader có thể thấy chuỗi đang bị cập nhật.
- Reload lỗi sau một số field vẫn có thể làm thay đổi state.

### Yêu cầu sửa plan

Plan phải yêu cầu quy trình:

1. Copy default/current profile sang profile tạm.
2. Parse toàn bộ vào profile tạm.
3. Validate mọi field và cross-field invariant.
4. Nếu có lỗi, bỏ toàn bộ profile tạm và giữ profile đang active.
5. Publish profile mới bằng cơ chế đồng bộ phù hợp.
6. Chỉ chạy các consumer patcher sau khi publish thành công.

Các invariant tối thiểu:

- Samsung serial đúng độ dài/charset.
- IMEI đúng độ dài và Luhn nếu IMEI vẫn thuộc scope.
- MAC hợp lệ, không multicast, không all-zero/all-FF.
- Ngày đúng `YYYY-MM-DD` và hợp lệ theo lịch.
- UFS model/revision đúng giới hạn output.
- Các trường liên kết AP serial, EM DID và unique ID nhất quán.

## 9. Verification Plan cần viết lại

Build đủ sáu package và kiểm tra hash chỉ chứng minh packaging hoàn thành, không chứng minh code đúng.

Plan mới cần ít nhất các gate sau.

### Gate A — Source integrity

- Xác định commit/baseline chính xác.
- Ghi manifest file thay đổi.
- Không còn object/header bị reference nhưng không tồn tại.
- `git diff --check` sạch.
- Artifact phải ghi source tree hash hoặc patch digest.

### Gate B — Static/build validation

- Clean build Vanilla.
- Clean build KernelSU/SuSFS nếu profile này còn thuộc scope.
- Không tái sử dụng object cũ.
- Check build log cho warning/error/undefined symbol.
- Chạy checkpatch/static checker cho các file sửa.
- Kiểm tra cấu hình cuối, không chỉ config fragment.

### Gate C — Unit and failure-path tests

- Config missing, empty, malformed và oversized.
- Invalid serial/IMEI/MAC/date/numeric values.
- Partial read với nhiều buffer size và offset.
- EOF và repeated read.
- Concurrent readers trong lúc reload.
- Allocation/copy failure handling.
- Filesystem chưa mount và mount trễ.

### Gate D — Read-only runtime audit

- Boot nhiều lần và kiểm tra profile stability.
- Kiểm tra cả root và UID thường.
- Kiểm tra API thực mà Android/framework/app sử dụng, không chỉ endpoint riêng của Ghost.
- Kiểm tra kernel log cho warning/oops/stall.
- Kiểm tra camera, modem, Wi-Fi, Bluetooth, USB và storage vẫn hoạt động.
- Đo overhead của read path.
- Không thay đổi thời gian, filesystem hoặc flash state trong audit.

### Gate E — Zero-disk proof

- Liệt kê mọi write call do Ghost thực hiện.
- Chứng minh không còn write path tới EFS/data nếu mode là RAM-only.
- Không xác minh dữ liệu gốc qua cùng interceptor đang che dữ liệu.
- Nếu không thể chứng minh, không được dùng nhãn zero-disk.

### Gate F — Artifact verification

- Mỗi artifact phải gắn với source manifest duy nhất.
- Hash trong tài liệu phải khớp file thực tế.
- Tên profile phải khớp config cuối.
- Tách rõ Vanilla và KernelSU/SuSFS.
- Không coi artifact cũ từ cây WSL khác là bằng chứng cho working tree Windows hiện tại.

## 10. Thứ tự kế hoạch mới đề xuất

Hãy viết lại plan theo thứ tự sau:

### Phase 0 — Baseline recovery và evidence

- Không thêm tính năng.
- Xác định source chính xác tạo artifact gần nhất.
- Giải quyết các reference tới ghost components đã bị xóa.
- Tạo clean build reproducible tối thiểu cho Vanilla.
- Thu log read-only chứng minh nguyên nhân profile fallback.

### Phase 1 — Chốt phạm vi và invariants

- Lập bảng surface/source/consumer/test.
- Quyết định RAM-only session identity hay persistent identity.
- Không tuyên bố cùng lúc zero-disk và persistent-on-disk.
- Loại bỏ security-patch spoof khỏi tiêu chí bảo mật.

### Phase 2 — Profile loader correctness

- Chỉ sửa sau khi có evidence từ Phase 0.
- Parse/validate/publish atomically.
- Có telemetry lỗi rõ ràng.
- Không ghi EFS.

### Phase 3 — Một owner cho mỗi output surface

- Serial trước.
- Battery sau.
- UFS sau khi inventory hoàn tất.
- Network/telephony chỉ sau khi ghost_net architecture được quyết định.
- Không triển khai nhiều surface trong cùng một bước chưa kiểm chứng.

### Phase 4 — Verification và regression

- Chạy đầy đủ Gate A–F.
- So sánh với baseline V100.
- Dừng nếu có regression boot, storage, modem, camera, USB hoặc network.

### Phase 5 — Packaging

- Chỉ đóng gói sáu artifact sau khi source và runtime gates đều PASS.
- Ghi source manifest và hash mới vào tài liệu.

## 11. Tiêu chí để plan mới được chấp thuận

Plan sửa lại chỉ được coi là hợp lý khi đáp ứng đầy đủ:

- Không mô tả lại code đã tồn tại như thay đổi mới.
- Không tham chiếu file đã bị xóa mà không có bước giải quyết baseline.
- Có nguyên nhân đã được chứng minh cho lỗi profile fallback.
- Không còn write path tới EFS nếu tuyên bố zero-disk.
- Có semantics chính xác cho partial read/offset/EOF/error.
- Có chiến lược đồng bộ và atomic profile publication.
- Phân biệt sysfs display override với underlying hardware identity.
- Không dùng `/proc/ghost_imei` để đại diện cho Android Telephony.
- Có owner và test riêng cho Bluetooth nếu Bluetooth còn trong scope.
- Không trình bày property security patch giả như bản vá CVE thật.
- Có clean reproducible build trước runtime test.
- Có artifact provenance và hash đồng nhất.
- Có rollback/stop criteria rõ ràng.

## Chỉ thị cuối cho Antigravity

Không sửa code dựa trên `implementation_plan.md` hiện tại.

Hãy tạo một bản implementation plan mới, bắt đầu từ Phase 0, sử dụng source hiện tại làm bằng chứng. Với mọi proposed modification, ghi rõ:

```text
Observed evidence
Root cause status: proven / hypothesis
Exact owner/source surface
Smallest intended change
Failure modes
Static/build test
Runtime read-only test
Rollback or stop condition
```

Nếu root cause chưa proven, chỉ được đề xuất instrumentation/diagnostic read-only; không được đề xuất bản sửa chức năng như thể nguyên nhân đã chắc chắn.
