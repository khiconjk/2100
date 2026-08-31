#!/usr/bin/env bash
# ==============================================================================
# SETUP HYBRID KEYBOX & TRICKY STORE FOR SAMSUNG GALAXY S21 (SM-G991B / o1s)
# ==============================================================================
# Purpose:
#   Configures Tricky Store on device to eliminate 0x00 in Hardware Key Attestation.
#   Uses the extracted genuine Samsung StrongBox OEM root key hash:
#   78d88bcb03734bebc53a14658b315a500f7c7488357dafe490d283cc726bff95
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

printf '=== HYBRID DEFENSE: TRICKY STORE AUTO-CONFIGURATION ===\n'

# 1. Ensure device is connected and rooted
if ! adb_cmd get-state >/dev/null 2>&1; then
    printf 'Error: Device not detected via ADB.\n' >&2
    exit 1
fi

device_model=$(adb_cmd shell getprop ro.product.model | tr -d '\r\n')
printf 'Connected Device: %s\n' "$device_model"

# 2. Check Tricky Store installation directory
TRICKY_DIR="/data/adb/tricky_store"
adb_su "mkdir -p $TRICKY_DIR && chmod 755 $TRICKY_DIR"

# 3. Configure boot_key (Genuine StrongBox OEM Root Key Hash for Samsung S21)
BOOT_KEY="78d88bcb03734bebc53a14658b315a500f7c7488357dafe490d283cc726bff95"
adb_su "printf '%s\n' '$BOOT_KEY' > $TRICKY_DIR/boot_key"
adb_su "chmod 644 $TRICKY_DIR/boot_key"
printf '[✔] Configured boot_key: %s\n' "$BOOT_KEY"

# 4. Configure boot_hash (Stock VBMeta SHA-256 Digest)
BOOT_HASH="22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781"
adb_su "printf '%s\n' '$BOOT_HASH' > $TRICKY_DIR/boot_hash"
adb_su "printf '%s\n' '$BOOT_HASH' > /data/adb/boot_hash 2>/dev/null || true"
adb_su "chmod 644 $TRICKY_DIR/boot_hash"
printf '[✔] Configured boot_hash: %s\n' "$BOOT_HASH"

# 5. Configure target.txt with strict leaf certificate spoofing (!)
adb_su "cat << 'EOF' > $TRICKY_DIR/target.txt
com.google.android.gms!
com.google.android.gsf!
com.reveny.nativecheck!
io.github.vvb2060.keyattestation!
com.vnpay.bidv!
com.mbmobile!
com.VCB!
com.vnid!
EOF
chmod 644 $TRICKY_DIR/target.txt"
printf '[✔] Configured target.txt for detection apps and banking packages.\n'

# 6. Check for keybox.xml presence
if adb_su "test -f $TRICKY_DIR/keybox.xml"; then
    printf '[✔] keybox.xml is PRESENT in %s\n' "$TRICKY_DIR"
else
    printf '[!] NOTE: %s/keybox.xml is not yet present.\n' "$TRICKY_DIR"
    printf '    To achieve Strong Integrity, place your valid keybox.xml into %s/keybox.xml\n' "$TRICKY_DIR"
fi

printf '=== SETUP COMPLETED SUCCESSFULLY ===\n'
