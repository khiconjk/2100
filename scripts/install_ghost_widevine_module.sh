#!/usr/bin/env bash
# ==============================================================================
# INSTALL GHOST WIDEVINE MODULE TO DEVICE VIA ADB
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
    adb_cmd shell "su -c $(printf '%q' "$cmd")"
}

printf '=== CÀI ĐẶT MODULE GHOST WIDEVINE SPOOF (LỚP 8) ===\n'

if ! adb_cmd get-state >/dev/null 2>&1; then
    printf 'Lỗi: Thiết bị chưa kết nối qua ADB.\n' >&2
    exit 1
fi

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root_dir"
module_src="modules/ghost_widevine"
target_dir="/data/adb/modules/ghost_widevine"

printf 'Đang đẩy module từ %s lên thiết bị...\n' "$module_src"
adb_su "mkdir -p $target_dir"

adb_cmd push "$module_src/module.prop" /data/local/tmp/
adb_cmd push "$module_src/service.sh" /data/local/tmp/
adb_cmd push "$module_src/post-fs-data.sh" /data/local/tmp/
adb_cmd push "$module_src/sepolicy.rule" /data/local/tmp/

adb_su "cp /data/local/tmp/module.prop $target_dir/"
adb_su "cp /data/local/tmp/service.sh $target_dir/"
adb_su "cp /data/local/tmp/post-fs-data.sh $target_dir/"
adb_su "cp /data/local/tmp/sepolicy.rule $target_dir/"

adb_su "chmod 755 $target_dir/service.sh"
adb_su "chmod 755 $target_dir/post-fs-data.sh"
adb_su "chmod 644 $target_dir/module.prop"
adb_su "chmod 644 $target_dir/sepolicy.rule"

# Dọn dẹp tệp tạm
adb_su "rm -f /data/local/tmp/module.prop /data/local/tmp/service.sh /data/local/tmp/post-fs-data.sh /data/local/tmp/sepolicy.rule"

printf '[✔] Đã cài đặt thành công module ghost_widevine vào %s\n' "$target_dir"
printf '[*] Module sẽ tự động kích hoạt trong lần khởi động tiếp theo của KernelSU.\n'
