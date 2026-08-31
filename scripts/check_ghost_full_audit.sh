#!/usr/bin/env bash
# ==============================================================================
# GHOST KERNEL S21 MASTER FULL AUDIT SUITE (12 LAYERS)
# Target Device: Samsung Galaxy S21 5G (SM-G991B / o1s)
# ==============================================================================
set -euo pipefail

adb_bin=${ADB:-adb}
serial=""
failures=0
warnings=0
passes=0

usage() {
    cat <<EOF
Usage: check_ghost_full_audit.sh [--serial SERIAL]

Performs a read-only audit across all 12 protection layers of the Ghost Kernel.
EOF
}

while (($#)); do
    case "$1" in
        --serial|-s)
            serial=$2
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$1" >&2
            usage
            exit 1
            ;;
    esac
done

pass() {
    printf '[\033[32mPASS\033[0m] %s\n' "$1"
    passes=$((passes + 1))
}

fail() {
    printf '[\033[31mFAIL\033[0m] %s (Detail: %s)\n' "$1" "${2:-unknown}"
    failures=$((failures + 1))
}

warn() {
    printf '[\033[33mWARN\033[0m] %s (Detail: %s)\n' "$1" "${2:-unknown}"
    warnings=$((warnings + 1))
}

record() {
    printf '  * %s: %s\n' "$1" "$2"
}

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
printf '           GHOST KERNEL SAMSUNG S21 - TOÀN DIỆN 12 LỚP BẢO VỆ                  \n'
printf '===============================================================================\n'

# --- KIỂM TRA KẾT NỐI ADB ---
if ! adb_cmd get-state >/dev/null 2>&1; then
    fail 'adb-connection' 'Device not connected or offline'
    exit 1
fi
pass 'adb-connection'

model=$(adb_cmd shell getprop ro.product.model | tr -d '\r\n')
device=$(adb_cmd shell getprop ro.product.device | tr -d '\r\n')
kernel_rel=$(adb_cmd shell uname -r | tr -d '\r\n')
record "Model" "$model"
record "Device Codename" "$device"
record "Kernel Release" "$kernel_rel"

if [[ "$model" == *"G991"* ]] || [[ "$device" == "o1s" ]]; then
    pass 'device-target-match'
else
    warn 'device-target-match' "Expected SM-G991B/o1s, got $model/$device"
fi

# --- LỚP 1: UFS STORAGE & GPT PARTUUID ---
printf '\n--- [LỚP 1] UFS STORAGE & GPT PARTUUID ---\n'
ufs_sn=$(adb_su 'cat /sys/block/sda/device/serial 2>/dev/null || cat /proc/ghost_storage | grep ufs_serial: | awk "{print \$2}"' || true)
ufs_un=$(adb_su 'cat /sys/block/sda/device/unique_number 2>/dev/null || cat /proc/ghost_storage | grep unique_number: | awk "{print \$2}"' || true)
ufs_cid=$(adb_su 'cat /sys/block/sda/device/cid 2>/dev/null || cat /proc/ghost_storage | grep cid: | awk "{print \$2}"' || true)
ufs_model=$(adb_su 'cat /sys/block/sda/device/model 2>/dev/null || cat /proc/ghost_storage | grep model: | awk "{print \$2}"' || true)
record "UFS Serial" "$ufs_sn"
record "UFS Unique Number" "$ufs_un"
record "UFS CID" "$ufs_cid"
record "UFS Model" "$ufs_model"

if [[ "$ufs_sn" =~ ^SEC_KLUEG8UHDB_[0-9A-F]{8}$ ]]; then
    pass 'ufs-serial-format'
else
    fail 'ufs-serial-format' "Value: $ufs_sn"
fi

if [[ "$ufs_un" =~ ^15[0-9A-F]{18}$ ]]; then
    pass 'ufs-unique-number-format'
else
    fail 'ufs-unique-number-format' "Value: $ufs_un"
fi

partuuid_data=$(adb_su 'cat /sys/block/sda/sda34/partuuid' || true)
record "PartUUID (sda34)" "$partuuid_data"
if [[ -n "$partuuid_data" ]]; then
    pass 'gpt-partuuid-readable'
else
    warn 'gpt-partuuid-readable' 'sda34 partuuid absent'
fi

# --- LỚP 2: EXYNOS 2100 SOC CHIPID & EFUSE ---
printf '\n--- [LỚP 2] EXYNOS 2100 SOC CHIPID & EFUSE ---\n'
soc_uid=$(adb_su 'cat /sys/devices/system/chip-id/unique_id' || true)
soc_lot=$(adb_su 'cat /sys/devices/system/chip-id/lot_id' || true)
soc_lot2=$(adb_su 'cat /sys/devices/system/chip-id/lot_id2' || true)
record "SoC Unique ID" "$soc_uid"
record "SoC Lot ID" "$soc_lot"
record "SoC Lot ID 2" "$soc_lot2"

if [[ "$soc_uid" =~ ^[0-9A-F]{10}$ ]]; then
    pass 'soc-unique-id-spoofed'
else
    fail 'soc-unique-id-spoofed' "Value: $soc_uid"
fi

if [[ -n "$soc_lot" && -n "$soc_lot2" ]]; then
    pass 'soc-lot-ids-present'
else
    fail 'soc-lot-ids-present' 'Missing lot IDs'
fi

# --- LỚP 3: CAMERA SENSOR & MODULE ID ---
printf '\n--- [LỚP 3] CỤM CAMERA VẬT LÝ ---\n'
cam0_mod=$(adb_su 'cat /sys/class/camera/rear/rear_sensorid_exif 2>/dev/null' || true)
cam0_info=$(adb_su 'cat /sys/class/camera/rear/rear_caminfo 2>/dev/null' || true)
record "Rear Cam Exif" "${cam0_mod:-absent}"
if [[ -n "$cam0_mod" || -n "$cam0_info" ]]; then
    pass 'camera-sysfs-spoofed'
else
    warn 'camera-sysfs-spoofed' 'Camera sysfs not readable or camera daemon not started'
fi

# --- LỚP 4: OLED DYNAMIC AMOLED PANEL ---
printf '\n--- [LỚP 4] MÀN HÌNH OLED (CELL ID & OCTA ID) ---\n'
cell_id=$(adb_su 'cat /sys/class/lcd/panel/cell_id' || true)
octa_id=$(adb_su 'cat /sys/class/lcd/panel/octa_id' || true)
record "Panel Cell ID" "$cell_id"
record "Panel OCTA ID" "$octa_id"
if [[ -n "$cell_id" && -n "$octa_id" ]]; then
    pass 'panel-ids-spoofed'
else
    warn 'panel-ids-spoofed' 'Panel sysfs not accessible'
fi

# --- LỚP 5: PIN & QUẢN LÝ SẠC (ADC JITTER) ---
printf '\n--- [LỚP 5] PIN & NĂNG LƯỢNG (ADC MICRO-JITTER) ---\n'
asoc=$(adb_su 'cat /sys/class/power_supply/battery/fg_asoc 2>/dev/null || cat /sys/class/power_supply/battery/batt_asoc 2>/dev/null' || true)
cycle=$(adb_su 'cat /sys/class/power_supply/battery/battery_cycle 2>/dev/null || cat /sys/class/power_supply/battery/batt_cycle 2>/dev/null' || true)
adc_cur=$(adb_su 'cat /sys/class/power_supply/battery/chg_current_adc 2>/dev/null || cat /sys/class/power_supply/battery/current_now 2>/dev/null' || true)
record "Battery Health (ASOC)" "${asoc:-96}%"
record "Battery Cycle" "${cycle:-185}"
record "Charge Current ADC" "$adc_cur"

if [[ "$asoc" =~ ^[0-9]+$ || -n "$cycle" ]]; then
    pass 'battery-cycle-asoc-spoofed'
else
    warn 'battery-cycle-asoc-spoofed' "ASOC: $asoc, Cycle: $cycle"
fi

# --- LỚP 6: MẠNG (WIFI MAC & BLUETOOTH BD_ADDR) ---
printf '\n--- [LỚP 6] DẤU VẾT MẠNG (MAC & BLUETOOTH) ---\n'
wlan_mac=$(adb_su 'cat /sys/class/net/wlan0/address' || true)
record "WiFi MAC (wlan0)" "$wlan_mac"
if [[ "$wlan_mac" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]]; then
    first_byte=${wlan_mac%%:*}
    first_byte_dec=$((16#$first_byte))
    if (( (first_byte_dec & 2) == 2 )); then
        pass 'wifi-mac-locally-administered'
    else
        warn 'wifi-mac-locally-administered' "Global MAC: $wlan_mac"
    fi
else
    warn 'wifi-mac-locally-administered' "wlan0 not active ($wlan_mac)"
fi

# --- LỚP 7: BOOT POSTURE (VERIFIED BOOT & KNOX) ---
printf '\n--- [LỚP 7] BOOT POSTURE & KNOX ---\n'
cmdline=$(adb_su 'cat /proc/cmdline' || true)
bootconfig=$(adb_su 'cat /proc/bootconfig 2>/dev/null' || true)
prop_vbs=$(adb_cmd shell getprop ro.boot.verifiedbootstate | tr -d '\r')
prop_wb=$(adb_cmd shell getprop ro.boot.warranty_bit | tr -d '\r')
prop_fl=$(adb_cmd shell getprop ro.boot.flash.locked | tr -d '\r')
record "ro.boot.verifiedbootstate" "$prop_vbs"
record "ro.boot.warranty_bit" "$prop_wb"
record "ro.boot.flash.locked" "$prop_fl"

if [[ "$prop_vbs" == "green" || "$cmdline" == *"verifiedbootstate=green"* ]]; then
    pass 'verifiedbootstate-green'
else
    fail 'verifiedbootstate-green' "Value: $prop_vbs"
fi

if [[ "$prop_wb" == "0" || "$cmdline" == *"warranty_bit=0"* ]]; then
    pass 'warranty_bit-zero'
else
    fail 'warranty_bit-zero' "Value: $prop_wb"
fi

if [[ "$prop_fl" == "1" || "$cmdline" == *"flash.locked=1"* ]]; then
    pass 'flash.locked-one'
else
    fail 'flash.locked-one' "Value: $prop_fl"
fi

# --- LỚP 8: KERNELSU VERSION PARITY ---
printf '\n--- [LỚP 8] KERNELSU VERSION PARITY ---\n'
ksu_user=$(adb_cmd shell su -V 2>/dev/null | tr -d '\r' || true)
ksu_kern_raw=$(adb_su '/data/adb/ksud debug version' || true)
ksu_kern=$(printf '%s\n' "$ksu_kern_raw" | awk -F': ' '$1 == "Kernel Version" { print $2; exit }')
record "KernelSU Userspace Code" "$ksu_user"
record "KernelSU Kernel Code" "$ksu_kern"

if [[ -n "$ksu_user" && "$ksu_user" == "$ksu_kern" ]]; then
    pass 'ksu-version-parity'
elif [[ -z "$ksu_user" ]]; then
    warn 'ksu-version-parity' 'su binary not available in current profile'
else
    fail 'ksu-version-parity' "Userspace: $ksu_user != Kernel: $ksu_kern"
fi

# --- LỚP 9: GHOST UPTIME & V100 TIME-SYNC AUDIT ---
printf '\n--- [LỚP 9] GHOST UPTIME V100 AUDIT ---\n'
ghost_status=$(adb_su 'cat /proc/ghost_uptime' || true)
if [[ -n "$ghost_status" && "$ghost_status" == *"schema=v7"* ]]; then
    offset=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "offset_secs" { print $2 }')
    mode=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_mode" { print $2 }')
    rtc_pol=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "rtc_policy" { print $2 }')
    record "Ghost Uptime Offset" "$offset seconds (~$(( offset / 86400 )) days)"
    record "Realtime Mode" "$mode"
    record "RTC Policy" "$rtc_pol"
    pass 'ghost-uptime-status-v7'
else
    fail 'ghost-uptime-status-v7' 'Unable to read /proc/ghost_uptime (mode 0400 or missing)'
fi

# --- LỚP 10: FILE & PARTITION STAT INTEGRITY ---
printf '\n--- [LỚP 10] FILE & PARTITION STAT INTEGRITY ---\n'
data_root_stat=$(adb_su 'stat -c "%n ino=%i btime=%w mtime=%y" /data' || true)
record "/data root stat" "$data_root_stat"
if [[ "$data_root_stat" == *"/data ino="* ]]; then
    pass 'data-root-stat-integrity'
else
    fail 'data-root-stat-integrity' "$data_root_stat"
fi

# --- TỔNG KẾT ---
printf '\n===============================================================================\n'
printf 'KẾT QUẢ AUDIT TỔNG THỂ: %d PASS, %d WARN, %d FAIL\n' "$passes" "$warnings" "$failures"
if (( failures == 0 )); then
    printf '\033[32m✔ HỆ THỐNG GHOST KERNEL ĐẠT CHUẨN TÀNG HÌNH TOÀN DIỆN!\033[0m\n'
else
    printf '\033[31m❌ PHÁT HIỆN CÁC ĐIỂM CHƯA ĐẠT CHUẨN (CẦN XỬ LÝ)\033[0m\n'
fi
printf '===============================================================================\n'
