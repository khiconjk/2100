#!/usr/bin/env bash
# ==============================================================================
# PACKAGE GHOST KERNEL ALL-IN-ONE ANYKERNEL3 FLASHABLE ZIP
# Target Device: Samsung Galaxy S21 5G (SM-G991B / o1s)
# ==============================================================================
set -euo pipefail

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root_dir"

AK3_DIR="$root_dir/AnyKernel3"
OUTPUT_ZIP="$root_dir/AnyKernel3-o1s-ghost-all-in-one.zip"

printf '===============================================================================\n'
printf '        ĐÓNG GÓI GHOST KERNEL V101 ALL-IN-ONE (FLASH 1 FILE DUY NHẤT)          \n'
printf '===============================================================================\n'

# 1. Kiểm tra các tệp phân vùng bắt buộc
for img in boot.img vendor_boot.img dtbo.img; do
    if [ ! -f "$AK3_DIR/$img" ]; then
        if [ -f "build/out/o1s/$img" ]; then
            cp "build/out/o1s/$img" "$AK3_DIR/$img"
            printf '[✔] Đã sao chép %s từ build/out/o1s vào AnyKernel3\n' "$img"
        else
            printf '[\033[31mFAIL\033[0m] Thiếu tệp phân vùng: %s/%s\n' "$AK3_DIR" "$img" >&2
            exit 1
        fi
    else
        printf '[✔] Tìm thấy %s (Kích thước: %s bytes)\n' "$img" "$(stat -c%s "$AK3_DIR/$img" 2>/dev/null || wc -c < "$AK3_DIR/$img")"
    fi
done

# 2. Đồng bộ các module mới nhất vào payload
printf '\n[*] Đồng bộ module payload vào AnyKernel3...\n'
mkdir -p "$AK3_DIR/payload/modules/ghost_widevine"
cp -rf "$root_dir/modules/ghost_widevine/"* "$AK3_DIR/payload/modules/ghost_widevine/"
printf '[✔] Đã cập nhật Ghost Widevine Layer 8 vào payload.\n'

# 3. Tạo file ZIP All-In-One
printf '\n[*] Đang nén tệp ZIP AnyKernel3 All-In-One...\n'
rm -f "$OUTPUT_ZIP"

pushd "$AK3_DIR" > /dev/null
zip -r9 "$OUTPUT_ZIP" * \
    -x ".git*" \
    -x "README.md" \
    -x "*placeholder" \
    -x "LICENSE"
popd > /dev/null

printf '\n===============================================================================\n'
printf '[\033[32mSUCCESS\033[0m] ĐÃ TẠO THÀNH CÔNG GÓI FLASH DUY NHẤT:\n'
printf '  👉 Đường dẫn: %s\n' "$OUTPUT_ZIP"
printf '  👉 Kích thước: %s bytes (~%s MB)\n' \
    "$(stat -c%s "$OUTPUT_ZIP" 2>/dev/null || wc -c < "$OUTPUT_ZIP")" \
    "$(du -m "$OUTPUT_ZIP" 2>/dev/null | awk '{print $1}' || echo "31")"
printf '===============================================================================\n'
