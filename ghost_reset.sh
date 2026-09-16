#!/system/bin/sh
# ====================================================================
#   GHOST KERNEL: UNIFIED HARDWARE & USERSPACE PROFILE REROLL V7
#   FULL 50-PILLAR COMBO (PILLARS 46, 47, 54, 72, 73 INTEGRATED)
# ====================================================================

echo "============================================================="
echo "       GHOST KERNEL UNIFIED PROFILE RESET ENGINE V7          "
echo "============================================================="

# 0. Locate resetprop FIRST (BUG-06 fix: must be before any resetprop calls)
RESETPROP=$(command -v resetprop 2>/dev/null)
if [ -z "$RESETPROP" ]; then
    [ -x /data/adb/ksu/bin/resetprop ] && RESETPROP="/data/adb/ksu/bin/resetprop"
    [ -x /data/adb/magisk/resetprop ] && RESETPROP="/data/adb/magisk/resetprop"
    [ -x /data/adb/ap/bin/resetprop ] && RESETPROP="/data/adb/ap/bin/resetprop"
fi
if [ -z "$RESETPROP" ]; then
    echo "[!] WARNING: resetprop not found. Property updates will be skipped."
fi

# Helper: safe random integer (BUG-04 fix: prevent pipeline crash on empty od)
ghost_rand() {
    _raw=$(od -An -tu4 -N4 /dev/urandom 2>/dev/null | tr -d ' ')
    _raw=${_raw:-0}
    echo $(( _raw % $1 ))
}

# 1. Trigger Kernel Level Master Seed & Hardware Reroll
echo "[*] Triggering Kernel Hardware Identity Reroll..."
[ -w /proc/ghost_storage ] && echo reroll > /proc/ghost_storage 2>/dev/null

# 1a. Parse arguments with proper while/shift loop (BUG-08 fix)
OPT_NEW_SERIAL=""
OPT_PROFILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --new-serial|--reroll-serial)
            OPT_NEW_SERIAL=1
            shift
            ;;
        --profile=*)
            OPT_PROFILE="${1#*=}"
            shift
            ;;
        --profile)
            OPT_PROFILE="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

# 1b. Samsung Serial Number: Check Seed-Marker Guard
SEED_FILE="/data/adb/.ghost_serial_seed"
SEED_BACKUP="/data/.ghost_serial_seed"
if [ -n "$OPT_NEW_SERIAL" ] || [ ! -s "$SEED_FILE" ]; then
    echo "[*] Randomizing Samsung Serial Number (Seed Marker)..."
    YEAR_ARR="R T W X Y"
    MONTH_ARR="1 2 3 4 5 6 7 8 9 A B C"
    r_rand_year=$(ghost_rand 5)
    r_rand_month=$(ghost_rand 12)
    idx=0; r_year="R"
    for y in $YEAR_ARR; do [ $idx -eq $r_rand_year ] && r_year="$y" && break; idx=$((idx + 1)); done
    idx=0; r_month="1"
    for m in $MONTH_ARR; do [ $idx -eq $r_rand_month ] && r_month="$m" && break; idx=$((idx + 1)); done
    # BUG-05 fix: use unbounded tr pipe for sufficient entropy
    r_body=$(tr -dc '0-9ABCDEFGHJKLMNPQRSTUVWXYZ' < /dev/urandom 2>/dev/null | head -c 7)
    [ ${#r_body} -lt 7 ] && r_body="8K3N9P2"
    ACTIVE_SERIAL="R5${r_year}${r_month}${r_body}"
    mkdir -p /data/adb 2>/dev/null
    printf '%s' "$ACTIVE_SERIAL" > "$SEED_FILE" 2>/dev/null
    printf '%s' "$ACTIVE_SERIAL" > "$SEED_BACKUP" 2>/dev/null
    chmod 644 "$SEED_FILE" "$SEED_BACKUP" 2>/dev/null
else
    ACTIVE_SERIAL=$(cat "$SEED_FILE" 2>/dev/null | tr -d ' \r\n\t')
    [ -z "$ACTIVE_SERIAL" ] && ACTIVE_SERIAL=$(cat "$SEED_BACKUP" 2>/dev/null | tr -d ' \r\n\t')
    [ -z "$ACTIVE_SERIAL" ] && ACTIVE_SERIAL=$(cat /efs/FactoryApp/serial_no 2>/dev/null | tr -d ' \r\n\t')
    echo "[*] Retaining Persistent Samsung Serial (Seed Guard): $ACTIVE_SERIAL"
fi

# 1c. AP Serial & EM DID Virtualization
# BUG-07 fix: generate exactly 12 hex chars to match kernel strlen==14 validation
if [ -n "$OPT_NEW_SERIAL" ] || [ ! -s "/data/.ghost_ap_seed" ]; then
    echo "[*] Randomizing AP Serial and EM DID..."
    NEW_AP_RAW=$(od -An -tx1 -N6 /dev/urandom 2>/dev/null | tr -d ' \n' | tr '[:lower:]' '[:upper:]')
    [ ${#NEW_AP_RAW} -lt 12 ] && NEW_AP_RAW="979BE420021A"
    NEW_AP_RAW=$(printf '%.12s' "$NEW_AP_RAW")
    NEW_AP_SERIAL="0x${NEW_AP_RAW}"
    NEW_EM_DID="20$(printf '%s' "$NEW_AP_RAW" | tr '[:upper:]' '[:lower:]')11"
    printf '%s' "$NEW_AP_SERIAL" > /data/.ghost_ap_seed 2>/dev/null
    printf '%s' "$NEW_AP_SERIAL" > /data/system/.ghost_ap_seed 2>/dev/null
    if [ -n "$RESETPROP" ]; then
        $RESETPROP -n ro.boot.ap_serial "$NEW_AP_SERIAL" 2>/dev/null
        $RESETPROP -n ro.boot.em.did "$NEW_EM_DID" 2>/dev/null
        $RESETPROP -n ro.boot.bore_cnt 3 2>/dev/null
    fi
else
    ACTIVE_AP=$(cat /data/.ghost_ap_seed 2>/dev/null | tr -d ' \r\n\t')
    if [ -n "$ACTIVE_AP" ] && [ -n "$RESETPROP" ]; then
        $RESETPROP -n ro.boot.ap_serial "$ACTIVE_AP" 2>/dev/null
        RAW_HEX=$(printf '%s' "$ACTIVE_AP" | tr '[:upper:]' '[:lower:]' | sed 's/^0x//')
        $RESETPROP -n ro.boot.em.did "20${RAW_HEX}11" 2>/dev/null
        $RESETPROP -n ro.boot.bore_cnt 3 2>/dev/null
    fi
fi

# 1d. Firmware Profile Rotation (Pool of 7 builds, all revision 4)
# BUG-15 fix: Removed Profile 0 (rev 3) to maintain Knox binary bit consistency
if [ -z "$OPT_PROFILE" ]; then
    OPT_PROFILE=$(ghost_rand 7)
fi

case "$OPT_PROFILE" in
    0)
        P_NAME="G991BXXU4BVA9 (Android 12 One UI 4.0 - Patch 2022-02-01)"
        P_BOOTLOADER="G991BXXU4BVA9"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXU4BVA9"
        P_SECURITY_PATCH="2022-02-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXU4BVA9:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXU4BVA9 release-keys"
        ;;
    1)
        P_NAME="G991BXXU4CVC4 (Android 12 One UI 4.1 - Patch 2022-03-01)"
        P_BOOTLOADER="G991BXXU4CVC4"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXU4CVC4"
        P_SECURITY_PATCH="2022-03-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXU4CVC4:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXU4CVC4 release-keys"
        ;;
    2)
        P_NAME="G991BXXS4CVDD (Android 12 One UI 4.1 - Patch 2022-05-01)"
        P_BOOTLOADER="G991BXXS4CVDD"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXS4CVDD"
        P_SECURITY_PATCH="2022-05-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXS4CVDD:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXS4CVDD release-keys"
        ;;
    3)
        P_NAME="G991BXXU4CVE7 (Android 12 One UI 4.1 - Patch 2022-06-01)"
        P_BOOTLOADER="G991BXXU4CVE7"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXU4CVE7"
        P_SECURITY_PATCH="2022-06-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXU4CVE7:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXU4CVE7 release-keys"
        ;;
    4)
        P_NAME="G991BXXS4CVGB (Android 12 One UI 4.1 - Patch 2022-07-01)"
        P_BOOTLOADER="G991BXXS4CVGB"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXS4CVGB"
        P_SECURITY_PATCH="2022-07-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXS4CVGB:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXS4CVGB release-keys"
        ;;
    5)
        P_NAME="G991BXXU4CVH7 (Android 12 One UI 4.1 - Patch 2022-09-01)"
        P_BOOTLOADER="G991BXXU4CVH7"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXU4CVH7"
        P_SECURITY_PATCH="2022-09-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXU4CVH7:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXU4CVH7 release-keys"
        ;;
    *)
        P_NAME="G991BXXU4CVJ1 (Android 12 One UI 4.1 - Patch 2022-10-01)"
        P_BOOTLOADER="G991BXXU4CVJ1"
        P_BUILD_ID="SP1A.210812.016"
        P_DISPLAY_ID="SP1A.210812.016.G991BXXU4CVJ1"
        P_SECURITY_PATCH="2022-10-01"
        P_FINGERPRINT="samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXU4CVJ1:user/release-keys"
        P_DESC="o1sxeea-user 12 SP1A.210812.016 SM-G991BXXU4CVJ1 release-keys"
        ;;
esac
echo "[*] Selected Firmware Profile: $P_NAME"

# 1e. Randomize UFS Model Variant
UFS_CHIP_ARR="KLUDG8UHDB-C2D1 KM2V7001CM-B706 THGJFGT1E45BAIL"
UFS_RAND_IDX=$(ghost_rand 3)
u_idx=0; ACTIVE_UFS_MODEL="KLUDG8UHDB-C2D1"
for chip in $UFS_CHIP_ARR; do
    [ $u_idx -eq $UFS_RAND_IDX ] && ACTIVE_UFS_MODEL="$chip" && break
    u_idx=$((u_idx + 1))
done
echo "[*] Selected UFS Chip Model: $ACTIVE_UFS_MODEL"

# Apply serial and firmware profile properties via resetprop
if [ -n "$RESETPROP" ]; then
    if [ -n "$ACTIVE_SERIAL" ] && [ ${#ACTIVE_SERIAL} -eq 11 ]; then
        $RESETPROP -n ro.serialno "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -n ro.boot.serialno "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -n gsm.sn1 "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -n ril.serialnumber "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -p ro.serialno "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -p ro.boot.serialno "$ACTIVE_SERIAL" 2>/dev/null
    fi

    $RESETPROP -n ro.bootloader "$P_BOOTLOADER" 2>/dev/null
    $RESETPROP -n ro.boot.bootloader "$P_BOOTLOADER" 2>/dev/null
    $RESETPROP -n ro.build.display.id "$P_DISPLAY_ID" 2>/dev/null
    $RESETPROP -n ro.build.version.incremental "$P_BOOTLOADER" 2>/dev/null
    $RESETPROP -n ro.build.version.security_patch "$P_SECURITY_PATCH" 2>/dev/null
    $RESETPROP -n ro.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -n ro.build.description "$P_DESC" 2>/dev/null

    $RESETPROP -n ro.vendor.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -n ro.bootimage.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -n ro.odm.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -n ro.product.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -n ro.system.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -n ro.system_ext.build.fingerprint "$P_FINGERPRINT" 2>/dev/null

    $RESETPROP -p ro.bootloader "$P_BOOTLOADER" 2>/dev/null
    $RESETPROP -p ro.boot.bootloader "$P_BOOTLOADER" 2>/dev/null
    $RESETPROP -p ro.build.display.id "$P_DISPLAY_ID" 2>/dev/null
    $RESETPROP -p ro.build.version.incremental "$P_BOOTLOADER" 2>/dev/null
    $RESETPROP -p ro.build.version.security_patch "$P_SECURITY_PATCH" 2>/dev/null
    $RESETPROP -p ro.build.fingerprint "$P_FINGERPRINT" 2>/dev/null
    $RESETPROP -p ro.build.description "$P_DESC" 2>/dev/null
fi

# BUG-03 fix: Incremental update of ghost.conf instead of destructive overwrite.
GHOST_CONF="/data/adb/ghost.conf"
if [ -d /data/adb ] && [ -f "$GHOST_CONF" ]; then
    sed -i \
        -e "s|^build_fingerprint[ =].*|build_fingerprint=$P_FINGERPRINT|" \
        -e "s|^build_desc[ =].*|build_desc=$P_DESC|" \
        -e "s|^build_id[ =].*|build_id=$P_BUILD_ID|" \
        -e "s|^security_patch[ =].*|security_patch=$P_SECURITY_PATCH|" \
        "$GHOST_CONF" 2>/dev/null
    echo 1 > /proc/ghost_reload 2>/dev/null
    echo "[*] Updated $GHOST_CONF (incremental) and reloaded Ghost Kernel"
elif [ -d /data/adb ]; then
    cat << 'ENDCONF' | sed \
        -e "s|__FP__|$P_FINGERPRINT|" \
        -e "s|__DESC__|$P_DESC|" \
        -e "s|__BID__|$P_BUILD_ID|" \
        -e "s|__SP__|$P_SECURITY_PATCH|" \
        > "$GHOST_CONF"
# Ghost Kernel Dynamic Profile Configuration
model=SM-G991B
product=o1sxeea
device=o1s
manufacturer=samsung
brand=samsung
soc_machine=Exynos
soc_family=samsung
build_fingerprint=__FP__
build_desc=__DESC__
build_id=__BID__
security_patch=__SP__
ENDCONF
    chmod 644 "$GHOST_CONF" 2>/dev/null
    echo 1 > /proc/ghost_reload 2>/dev/null
    echo "[*] Created $GHOST_CONF and reloaded Ghost Kernel"
fi

# Uptime companion: align batterystats file time to boot epoch.
if [ -f /data/system/batterystats.bin ]; then
    BTIME=$(awk '/^btime/{print $2}' /proc/stat)
    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        BT_FORMAT=$(date -d "@$BTIME" +%Y%m%d%H%M.%S 2>/dev/null)
        [ -z "$BT_FORMAT" ] && BT_FORMAT=$(date +%Y%m%d%H%M.%S)
        touch -t "$BT_FORMAT" /data/system/batterystats*.bin 2>/dev/null
    fi
fi

echo ""
echo "-------------------------------------------------------------"
echo "  GHOST RESET: Group 1 (Epoch HW) + Group 2 (Profile Rotation)"
echo "-------------------------------------------------------------"
echo "  Samsung Serial : ${ACTIVE_SERIAL:-unknown}"
echo "  Firmware Build : $P_BOOTLOADER ($P_SECURITY_PATCH)"
echo "  Fingerprint    : $P_FINGERPRINT"
echo "  UFS Model      : $ACTIVE_UFS_MODEL"
echo "-------------------------------------------------------------"
echo "[*] Unified reset and profile rotation completed"
echo "============================================================="
