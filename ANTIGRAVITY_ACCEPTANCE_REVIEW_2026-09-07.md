# Đánh giá nghiệm thu Ghost Kernel — 07/09/2026

## Kết luận

**CHƯA DUYỆT HOÀN THÀNH 100%.** Hai lỗi cũ về nested mutex và getter trả raw pointer đã được sửa ở mức kiểm tra source. MD5/SHA-256 của cả sáu artifact hiện tại đã được tính lại và khớp walkthrough. Tuy nhiên, còn vấn đề về tính nhất quán profile, validation, báo lỗi reload và bằng chứng nghiệm thu.

Tài liệu này cập nhật kết quả review trước. Không tiếp tục áp dụng kết luận cũ rằng sáu SHA-256 sai hoặc hai lỗi cụ thể nói trên vẫn còn nguyên.

Phạm vi kiểm tra: walkthrough tại `C:/Users/TUNG PC/.gemini/antigravity/brain/115fc5a4-90c4-4208-b577-9f1772a57ad0/walkthrough.md`, source và artifact tại `G:/ssS21-test-RTC-main`. Đây là kiểm tra source/tài liệu và checksum; không chạy lại test trên thiết bị trong lần review này. Kết quả runtime được mô tả là bằng chứng do báo cáo cung cấp, không phải phép đo độc lập mới.

## Những điểm đã xác nhận sửa

- `ghost_config_reload()` dùng `ghost_reload_mutex`; commit dùng `ghost_config_mutex`. Đường khóa lặp cùng mutex đã được loại bỏ.
- Các API getter cũ trả raw pointer đã được thay bằng copy-to-buffer/snapshot. Lỗi lifetime cụ thể của các getter cũ được đóng ở mức source.
- MD5 và SHA-256 của cả sáu gói G991B hiện tại khớp walkthrough tại thời điểm kiểm tra.
- Phạm vi mục tiêu đã thu hẹp thành pipeline spoofing/virtualization trong kernel. VPD/WWID vẫn ở HOLD/RESEARCH.

## Phát hiện cần xử lý

### P1 — Bản profile toàn cục vẫn có reader không được bảo vệ

Tại `kernel/ghost_config.c:711`, writer vẫn cập nhật `ghost_active_profile` bằng `memcpy`. Trong khi đó, các đường cập nhật properties/USB và worker vẫn đọc trực tiếp các field của struct này, ví dụ tại dòng 877 và 935.

RCU bảo vệ lifetime của object được publish qua pointer, không tự bảo vệ một struct global khác bị sửa tại chỗ. Reader có thể thấy dữ liệu đang được copy hoặc các field thuộc các phiên bản khác nhau khi reload diễn ra đồng thời.

Yêu cầu: lấy một snapshot profile nhất quán cho mỗi thao tác cập nhật liên quan; không tiếp tục đọc trực tiếp bản global mutable. Rà soát tất cả reference tới `ghost_active_profile`, kể cả fallback. Với các field liên quan nhau, không lấy từng field bằng nhiều snapshot độc lập nếu cần bảo đảm cùng phiên bản.

### P1 — Config lỗi có thể được chấp nhận một phần

Parser khởi tạo `temp_prof` từ profile hiện tại. Nhánh serial không hợp lệ bỏ qua giá trị đầu vào, giữ serial cũ. Kiểm tra cuối chỉ kiểm tra serial trong `temp_prof`, nên serial cũ hợp lệ có thể làm validation thành công dù file mới có serial sai. Các field hợp lệ khác của file mới vẫn được commit.

Các kết quả chuyển đổi số bằng `kstrtouint()` cũng chưa được kiểm tra đầy đủ. Điều này không đáp ứng tiêu chí đã đặt ra: config lỗi bị từ chối và không thay đổi active state.

Yêu cầu: định nghĩa rõ field bắt buộc/tùy chọn, trả lỗi đối với giá trị sai, và kiểm thử rollback toàn bộ trạng thái bao gồm kernel version. Nếu cố ý hỗ trợ bỏ qua field lỗi thì phải sửa đặc tả và trạng thái nghiệm thu tương ứng, không gọi đó là reject toàn bộ config lỗi.

### P2 — Proc reload không phản ánh lỗi về caller

Trong `ghost_reload_proc_write()` tại `kernel/ghost_config.c:865`, kết quả `ghost_config_reload()` chỉ được ghi log; hàm vẫn trả `count`. Vì vậy lệnh ghi proc thành công không tự chứng minh load/commit thành công.

Yêu cầu: truyền mã lỗi thích hợp về caller và chỉ áp dụng các bước hậu xử lý theo kết quả được thiết kế. Test phải xác nhận cả mã trả về, source status và dữ liệu profile mong đợi.

### P2 — Gate A chưa sạch như báo cáo

Kiểm tra working tree Windows vẫn cho lỗi trailing whitespace tại `arch/arm64/configs/o1s.config:115–121`.

Yêu cầu: kiểm tra lại chính tree dùng nghiệm thu. Nếu WSL và Windows khác nhau, xác lập tree chuẩn và chứng minh đồng bộ bằng hash. Không ghi `git diff --check` PASS khi tree được bàn giao vẫn báo lỗi.

### P2 — Partial serial lại xuất hiện trong walkthrough

Mục partial-read lại ghi trực tiếp các fragment serial. Dù chưa đủ để khôi phục toàn bộ serial, điều này không phù hợp với yêu cầu redact từng fragment trước khi chia sẻ.

Yêu cầu: dùng placeholder riêng cho từng chunk và giữ byte count/exit code. Không đưa giá trị định danh thật hoặc spoof thuộc phạm vi bảo mật vào báo cáo chia sẻ.

## Hiệu chỉnh cách diễn giải bằng chứng

### Reload tuần tự không phải kiểm thử concurrency

10 lần reload liên tiếp chứng minh đường reload đã hoàn tất trong những lần đo đó. Nó không chứng minh an toàn khi reader và reloader chạy đồng thời. Cần có cả reload/read đồng thời, cấu hình lỗi, kiểm tra trạng thái sau lỗi và kiểm tra thông báo lỗi kernel.

### SHA-256 EFS trước/sau không chứng minh không có write

Hash trùng nhau là bằng chứng không phát hiện khác biệt nội dung tại hai thời điểm lấy mẫu. Nó không loại trừ ghi cùng dữ liệu hoặc ghi rồi khôi phục. Kết quả chỉ trên EFS cũng không bao phủ `/data`.

Không gọi đây là “chứng minh toán học tuyệt đối không có bất kỳ lệnh ghi”. Để hỗ trợ yêu cầu zero intentional persistent writes, kết hợp inventory các đường ghi với runtime tracing có quy nguồn về Ghost và nêu rõ thời gian/phạm vi quan sát. Không đòi toàn bộ `/data` bất biến trong Android đang chạy vì thành phần khác có thể ghi hợp lệ.

### Clean build và reproducible build là hai tiêu chí khác nhau

Build thành công từ output sạch chưa chứng minh tái tạo cùng artifact. Cần lưu source tree identity, cấu hình, toolchain, lệnh build, log và kết quả build độc lập. Nếu chỉ kiểm tra clean build, đặt tên Gate đúng với bằng chứng đó.

### Device Tree và cmdline cần đối chiếu theo field

Báo cáo ghi Device Tree dài 2.766 bytes và cmdline dài 3.390 bytes. Không được mô tả hai nội dung giống nhau toàn bộ. Liệt kê các field cần đồng bộ và kết quả đối chiếu từng field. Hai endpoint Device Tree procfs/sysfs có thể so byte trực tiếp riêng.

### Root concealment cần giới hạn theo test đã chạy

`which su` và `stat /system/bin/su` bị chặn trên hai UID chỉ chứng minh các thao tác này trong các ngữ cảnh đã test. Không đủ để kết luận “ẩn root tuyệt đối”. Ghi rõ UID, SELinux context, namespace, lệnh và kết quả quan sát.

Security patch được walkthrough ghi là miễn kiểm theo chỉ thị người dùng. Không tính là PASS kỹ thuật; ghi rõ ngoại lệ và căn cứ phê duyệt. Review này không xác nhận độc lập chỉ thị đó. VPD/WWID tiếp tục HOLD, không tính là tính năng đã hoàn tất.

## Trạng thái Gate

| Gate | Đánh giá hiện tại |
|---|---|
| A — Source integrity | Chưa đạt tuyên bố tree sạch: còn lỗi diff check. |
| B — Reproducible builds | Chưa đủ bằng chứng tái tạo độc lập. |
| C — Failure/concurrency | Chưa đạt: reader global, validation và báo lỗi reload còn vấn đề. |
| D — Runtime audit | Đạt một phần theo báo cáo; cần giới hạn tuyên bố và bổ sung đối chiếu. |
| E — Zero persistent writes | Có bằng chứng EFS trước/sau; chưa chứng minh đầy đủ chính sách trên cả phạm vi. |
| F — Checksums/packaging | Đạt đối chiếu MD5/SHA-256 sáu artifact hiện tại; checksum không tự chứng minh artifact được build từ source đã review. |

## Điều kiện nghiệm thu tiếp theo

1. Chuyển toàn bộ reader liên quan sang snapshot nhất quán và kiểm tra đồng thời reload/read.
2. Xử lý lỗi parse/validation đúng hợp đồng; chứng minh input lỗi không làm thay đổi active state.
3. Trả lỗi reload đúng về caller và test cả nhánh thành công/thất bại.
4. Làm sạch diff check và gắn source tree identity với build cuối.
5. Lưu evidence build, test, provenance và phạm vi zero-write rõ ràng.
6. Redact fragment serial; sửa các câu khẳng định tuyệt đối vượt quá phép đo.
7. Nếu thay code, build và kiểm thử lại bản đó; sinh checksum mới từ artifact cuối cùng.

**Phán quyết:** Có thể đóng hai lỗi cụ thể cũ ở mức source và đóng lỗi bảng checksum. Task tổng thể vẫn chưa đủ điều kiện duyệt hoàn tất 100%.
