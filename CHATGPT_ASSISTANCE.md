# TIKTOK SHOP FORENSIC MONITOR & CHECKOUT ANALYZER
## HỒ SƠ DỰ ÁN & TÀI LIỆU KỸ THUẬT DÀNH CHO CHATGPT HỖ TRỢ

> **Mục đích tài liệu:** Cung cấp toàn bộ bối cảnh kỹ thuật, kiến trúc mã nguồn, cấu trúc dữ liệu logcat, mã lỗi và cơ chế hoạt động của công cụ `live_monitor.sh` trên Android để ChatGPT có thể nắm bắt ngay lập tức và tiếp tục hỗ trợ phát triển/tối ưu.

---

### 1. TỔNG QUAN HỆ THỐNG & MỤC TIÊU
- **Mục tiêu dự án:** Xây dựng script shell chạy trực tiếp trong ứng dụng **MT Manager** (Terminal) trên thiết bị Android Samsung đã Root để giám sát thời gian thực phiên đặt hàng (checkout) TikTok Shop (`com.ss.android.ugc.trill`).
- **Nhiệm vụ chính:**
  1. Phân tích trước các thông số đơn hàng ngay khi người dùng bấm mua: Giá thực trả (`price_val`), Voucher áp dụng, Quà tặng / Deal 1đ (`HitGift`), Trạng thái Freeship.
  2. Bắt ngay lập tức các cờ rủi ro (Risk Flags) và mã lỗi chặn đơn từ TikTok Shop trước khi người dùng thực hiện thanh toán.
  3. Cảnh báo bằng âm thanh (`\a`) và hiển thị bảng giải thích nguyên nhân + hướng xử lý cụ thể.
  4. Hiển thị thông tin giao diện gọn gàng, cố định, **độ rộng tối đa $\le 29$ ký tự/cột** để không bị vỡ giao diện hoặc tràn màn hình trong terminal MT Manager.

---

### 2. MÔI TRƯỜNG THIẾT BỊ & RÀNG BUỘC KỸ THUẬT
- **Thiết bị kiểm thử:** Samsung Galaxy S21 5G (SM-G991B), chạy **Android 12 (One UI 4.1)**.
- **Quyền hạn:** Đã root hoàn toàn (`uid=0(root)` qua KernelSU / Magisk / su).
- **Môi trường thực thi:**
  - Shell: Android toybox `/system/bin/sh` (mksh).
  - **Ràng buộc:** Không có Python, Bash đầy đủ hay Node.js trên thiết bị; toàn bộ phải viết bằng POSIX shell script thuần túy kết hợp các lệnh có sẵn của Android (`logcat`, `dumpsys`, `grep`, `sed`, `cut`, `tr`, `awk`, `cat`, `printf`).
- **Giao diện người dùng:** MT Manager Terminal (Cửa sổ terminal nhúng, độ phân giải hẹp, font monospace, yêu cầu mã màu ANSI chuẩn).

---

### 3. KIẾN TRÚC KỸ THUẬT CỦA SCRIPT (`live_monitor.sh v9.9`)

#### A. Quản lý trạng thái & IPC qua RamFS
- Tạo thư mục tạm trên RAM: `/dev/.tt_mon_${PID}` (fallback sang `/data/local/tmp/.tt_mon_${PID}`).
- Lưu trữ từng trường dữ liệu dưới dạng file phẳng nhỏ:
  - `v_net`, `t_net`: Giá trị & Trạng thái Mạng (OK/ERR/WAIT)
  - `v_dev`, `t_dev`: Trạng thái máy
  - `v_pho`, `v_add`: Số điện thoại & Địa chỉ nhận hàng
  - `v_prc`, `t_prc`: Giá sản phẩm (Ví dụ: `1đ (DEAL 1đ)`, `65000đ`)
  - `v_gft`, `t_gft`: Deal 1đ / Quà tặng (`Có (Deal 1đ)` hoặc `Không (Thường)`)
  - `v_vou`, `t_vou`: Voucher (`Áp dụng 2 mã`, `Deal 1đ Khách Mới`, `Cấm deal 1đ`)
  - `v_shp`, `t_shp`: Freeship (`Có Freeship` hoặc `Không Freeship`)
  - `v_err`, `t_err`: Mã lỗi (`Mã 90010`, `Mã 510010`, `Popup Chặn Đơn`, `Không có lỗi`)
  - `v_flg`, `t_flg`: Cờ sàn (`Ghim trợ giá`, `Cấm tân thủ`, `Sạch (Chưa cờ)`)
  - `v_ord`, `t_ord`: Trạng thái đơn (`Trang thanh toán`, `Bị chặn đơn`, `THÀNH CÔNG!`)
  - `exp_1`, `exp_2`, `exp_3`: 3 dòng giải thích chi tiết mã lỗi
  - `events.log`: 3 sự kiện gần nhất (ghi đè theo dạng hàng đợi vòng).

#### B. Cơ chế 2 Worker chạy ngầm
1. **`worker_env` (Quét môi trường & Cửa sổ chủ động mỗi 1s):**
   - Giám sát mạng: Đọc `/sys/class/net/` phát hiện `tun0/tun1/ppp0` (VPN) vs `wlan0` (Wi-Fi) vs `rmnet` (4G).
   - Đọc hồ sơ: `/sdcard/order_profile.txt` nếu có.
   - **Chủ động bắt cửa sổ (`dumpsys window`):**
     * Lọc `mCurrentFocus` và `mFocusedApp` (bỏ qua `null` để tránh lỗi One UI).
     * Khi phát hiện `OrderSubmitActivity`: Ngay lập tức gọi truy vấn `ActivityTaskManager:I` để nạp ngay giá và voucher.
     * Đếm số window của `OrderSubmitActivity`: Nếu `count > 1` (có popup chặn nổi đè lên), kích hoạt cảnh báo chặn đơn ngay lập tức.
2. **`worker_logcat` (Lắng nghe sự kiện logcat thời gian thực):**
   - Đọc trước các intent `aweme://ec/order_submit_v2` gần nhất.
   - Quét liên tục `logcat -v time` và gọi hàm `process_line`.

#### C. Cơ chế Single-Instance
- Tự động tìm và `kill -9` các tiến trình `live_monitor.sh` cũ chạy ngầm khi mở script mới, tránh xung đột màn hình.

---

### 4. DỮ LIỆU ĐẶC THÙ CỦA TIKTOK SHOP (LOGCAT FORENSICS)

#### A. URL Intent vào trang Checkout (`OrderSubmitActivity`)
Dòng log do hệ thống Android ghi nhận:
```log
ActivityTaskManager: START u0 {dat=aweme://ec/order_submit_v2?from_osp_starter=true&buy_type=0&...&bill_info_server_params={"GiftInfo":{"HitGift":false,...},"product_info":{"product_id":...,"sku_info_map":{"<sku_id>":{"price":{"price_val":"1","currency":"VND"}}}},"eligible_activity_info":{"UnqualifiedActivities":[...],"QualifiedActivities":[...]},"promotion_info":{"auto_claim_voucher_info":{"claim_vouchers":[{"voucher_type_id":...}]}}}...}
```

#### B. Phân loại cấu trúc khuyến mãi TikTok Shop:
1. **Deal Tân Thủ / Giá trực tiếp 1đ:**
   - `price_val`: `"1"`
   - `claim_vouchers`: `null` (Không có mã voucher nhập tay, giá giảm trực tiếp tại SKU).
   - Nếu đủ điều kiện: Không có `UnqualifiedActivities`.
   - Nếu bị cấm: `UnqualifiedActivities: [{"promotion_template": 510010}]`.
2. **Voucher Sàn & Shop:**
   - Nằm trong `claim_vouchers: [{"voucher_type_id": 7660402651137640200, ...}]`.
   - Hoặc `QualifiedActivities`:
     * Template `30000`: Voucher giảm giá Shop / Sàn.
     * Template `40010`: Voucher Miễn phí vận chuyển (Freeship).
     * Template `50000`: Khuyến mãi đặc quyền.

#### C. Bảng mã lỗi & Dấu hiệu nhận biết trong Logcat:
| Mã lỗi | Tên lỗi | Dấu hiệu trong Logcat | Nguyên nhân & Hướng xử lý |
| :--- | :--- | :--- | :--- |
| **510010 / 510011** | Cấm Deal Tân Thủ | `promotion_template": 510010` hoặc `510011` trong `UnqualifiedActivities` | Tài khoản hoặc thiết bị không còn quyền mua hàng 1đ mới. Cần đổi tài khoản hoặc xóa sạch data TikTok Shop. |
| **90010 / 90013** | Cấm Trợ Giá Sàn | `promotion_template": 90010` trong `UnqualifiedActivities` | Trùng IP, trùng địa chỉ giao hàng hoặc thiết bị bị ghim cờ lạm dụng voucher. Bỏ mã trợ giá sàn, xoay IP 4G. |
| **300013** | Hoạt Động Bất Thường | `Hoạt động bất thường`, `Xóa voucher` trong logcat | Hệ thống ghim cờ tài khoản có hành vi bất thường. Ngâm nick, đổi mạng sạch. |
| **POPUP CHẶN** | Modal Chặn Đơn | `GlobalHookDialog`, `TuxDialog`, `isFloating=true`, `ty=APPLICATION fmt=TRANSPARENT` | TikTok Risk Engine bật popup nổi từ chối thanh toán. Cần đổi thông tin người nhận, thiết bị hoặc IP. |
| **tpXXXX** | Lỗi App / Thiết bị | `tp1001`, `tp1002`, `tp...` | TikTok phát hiện can thiệp root/môi trường thiết bị. |

---

### 5. GIAO DIỆN CHUẨN TRÊN MT MANAGER (RỘNG 29 CỘT)
```text
┌─[ TIKTOK MONITOR v9.9 ]─┐
 Live MT Manager - Quét 1s  
├──────────────────────────┤
 Mạng   : Wi-Fi Sạch [OK] 
 Thiết bị: L1 Cloaked   [OK] 
 SĐT nhận: Theo tài khoản [OK] 
 Địa chỉ: Theo tài khoản [OK] 
 Giá SP : 1đ (DEAL 1đ) [ERR]
 Deal/Quà: Có (Deal 1đ) [ERR]
 Voucher : Deal 1đ Khách Mới [OK]
 Freeship: Có Freeship [OK] 
 Mã lỗi : Popup Chặn Đơn [ERR]
 Cờ sàn : Bị từ chối   [ERR]
 Đơn hàng: Bị chặn đơn  [ERR]
├─[ CHI TIẾT MÃ LỖI ]──────┤
 • Mã POPUP: Popup chặn đơn
 • Lý do : Risk Engine TikTok chặn
 • Xử lý : Đổi SĐT/ĐC hoặc xoay IP
├─[ SỰ KIỆN GẦN NHẤT ]─────┤
 22:50 [ERR] Bật popup chặn đơn!
 22:50 [TIN] Bấm đặt: Đang gửi...
 22:49 [OK]  Trang thanh toán
└──────────────────────────┘
 Nhấn Ctrl+C để dừng
```

---

### 6. CÁC HƯỚNG CẦN CHATGPT HỖ TRỢ TIẾP THEO
1. **Giải mã thêm các mã lỗi mới:** Bất kỳ mã lỗi dạng số (`600xxx`, `400xxx`) hoặc mã hex từ TikTok Shop khi người dùng gặp tình huống bị từ chối đơn mới.
2. **Nghiên cứu cơ chế Anti-Fraud / Risk Fingerprint:** Làm thế nào để che giấu hoặc đổi các thông số thiết bị (Android ID, GAID, DRM Widevine, MAC, Build prop) trên Samsung S21 Android 12 để tránh bị gắn cờ `510010` (hết quyền deal 1đ).
3. **Tối ưu hóa Script:** Đảm bảo script tốn ít tài nguyên CPU nhất trên thiết bị di động, không làm giật lag máy khi đang thao tác mua hàng.
