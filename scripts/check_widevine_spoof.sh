#!/usr/bin/env bash
# ==============================================================================
# AUDIT & VERIFY WIDEVINE DEVICE UNIQUE ID (LAYER 8)
# Target Device: Samsung Galaxy S21 5G (SM-G991B / o1s)
# ==============================================================================
set -euo pipefail

adb_bin=${ADB:-adb}
serial=${1:-}

if ! command -v "$adb_bin" >/dev/null 2>&1; then
    for candidate in \
        adb.exe \
        "/c/Users/TUNG PC/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe" \
        "$HOME/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe"; do
        if command -v "$candidate" >/dev/null 2>&1 || [[ -x "$candidate" ]]; then
            adb_bin=$candidate
            break
        fi
    done
fi

adb_cmd() {
    if [[ -n "$serial" ]]; then
        "$adb_bin" -s "$serial" "$@"
    else
        "$adb_bin" "$@"
    fi
}

adb_su() {
    local cmd=$1
    adb_cmd shell "su -c $(printf '%q' "$cmd")" 2>/dev/null | tr -d '\r' || true
}

printf '===============================================================================\n'
printf '        AUDIT & VERIFY LỚP 8: DRM WIDEVINE DEVICE UNIQUE ID                   \n'
printf '===============================================================================\n'

# 1. Kiểm tra kết nối ADB
if ! adb_cmd get-state >/dev/null 2>&1; then
    printf '[\033[31mFAIL\033[0m] Thiết bị không kết nối qua ADB.\n'
    exit 1
fi

device_model=$(adb_cmd shell getprop ro.product.model | tr -d '\r\n')
printf 'Thiết bị: %s\n' "$device_model"

# 2. Kiểm tra node /proc/ghost_widevine (Text Hex)
proc_wv=$(adb_su 'cat /proc/ghost_widevine' || true)
printf '\n--- [1] Node Kernel: /proc/ghost_widevine (Hex) ---\n'
if [[ -n "$proc_wv" && "$proc_wv" == *"widevine_device_id:"* ]]; then
    wv_id=$(printf '%s\n' "$proc_wv" | awk -F': ' '$1 == "widevine_device_id" { print $2 }')
    wv_len=$(printf '%s\n' "$proc_wv" | awk -F': ' '$1 == "length" { print $2 }')
    wv_mode=$(printf '%s\n' "$proc_wv" | awk -F': ' '$1 == "mode" { print $2 }')
    printf '[\033[32mPASS\033[0m] /proc/ghost_widevine tồn tại và hoạt động tốt.\n'
    printf '  * Ghost Widevine Device ID (Hex): %s\n' "$wv_id"
    printf '  * Length: %s bytes (%d hex chars)\n' "$wv_len" "${#wv_id}"
    printf '  * Mode: %s\n' "${wv_mode:-per-format}"
else
    printf '[\033[31mFAIL\033[0m] /proc/ghost_widevine không đọc được hoặc thiếu dữ liệu.\n'
    printf '  * Output: %s\n' "$proc_wv"
fi

# 3. Kiểm tra node /proc/ghost_widevine_raw (Binary Raw 32 Bytes)
printf '\n--- [2] Node Kernel: /proc/ghost_widevine_raw (Binary) ---\n'
raw_check=$(adb_su 'od -tx1 -An -N32 /proc/ghost_widevine_raw 2>/dev/null | tr -d " \n"' || true)
if [[ -n "$raw_check" && ${#raw_check} -eq 64 ]]; then
    printf '[\033[32mPASS\033[0m] /proc/ghost_widevine_raw cung cấp đủ 32 bytes binary nguyên bản.\n'
    printf '  * Raw Hex Stream: %s\n' "$raw_check"
    if [[ "$raw_check" == "$wv_id" ]]; then
        printf '[\033[32mPASS\033[0m] Khớp 100%% giữa node nhị phân và node hex text.\n'
    else
        printf '[\033[31mFAIL\033[0m] Bất đồng bộ giữa /proc/ghost_widevine và /proc/ghost_widevine_raw!\n'
    fi
else
    printf '[\033[33mWARN\033[0m] /proc/ghost_widevine_raw chưa sẵn sàng trên bản build kernel hiện tại của máy.\n'
    printf '  * Cần flash bản kernel mới nhất đã tích hợp ghost_widevine_raw.\n'
fi

# 4. Kiểm tra tiến trình Widevine DRM HAL
printf '\n--- [3] Kiểm tra Tiến trình Widevine DRM HAL ---\n'
wv_pid=$(adb_su 'pgrep -f android.hardware.drm.*widevine' || true)
if [[ -n "$wv_pid" ]]; then
    printf '[\033[32mPASS\033[0m] Tiến trình DRM HAL Widevine đang chạy (PID: %s).\n' "$wv_pid"
else
    printf '[\033[33mWARN\033[0m] Không tìm thấy tiến trình DRM HAL Widevine.\n'
fi

# 5. Kiểm tra Module Userspace Hook (KernelSU / Magisk)
printf '\n--- [4] Kiểm tra Trạng Thái Module Userspace Hook ---\n'
if adb_su 'test -d /data/adb/modules/ghost_widevine'; then
    printf '[\033[32mPASS\033[0m] Module ghost_widevine đã được cài đặt trong /data/adb/modules/ghost_widevine\n'
else
    printf '[\033[33mINFO\033[0m] Module ghost_widevine chưa được cài vào /data/adb/modules/.\n'
    printf '  * Chạy: bash scripts/install_ghost_widevine_module.sh để cài đặt tự động.\n'
fi

printf '\n===============================================================================\n'
printf 'KẾT THÚC KIỂM TRA LỚP 8\n'
printf '===============================================================================\n'
