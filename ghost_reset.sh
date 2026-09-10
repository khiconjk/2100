#!/system/bin/sh
# ====================================================================
#   GHOST KERNEL: UNIFIED HARDWARE & USERSPACE PROFILE REROLL V6
#   FULL 50-PILLAR COMBO (PILLARS 46, 47, 54, 72, 73 INTEGRATED)
# ====================================================================

echo "============================================================="
echo "       GHOST KERNEL UNIFIED PROFILE RESET ENGINE V6          "
echo "============================================================="

# 1. Trigger Kernel Level Master Seed & Hardware Reroll
echo "[*] Triggering Kernel Hardware & Identity Reroll..."
echo reroll > /proc/ghost_storage 2>/dev/null

# 1b. Samsung Serial Number: Check Seed-Marker Guard (Option 1)
# Keeps serial persistent across resets unless '--new-serial' or '--reroll-serial' is explicitly requested
SEED_FILE="/data/adb/.ghost_serial_seed"
SEED_BACKUP="/data/.ghost_serial_seed"
if [ "$1" = "--new-serial" ] || [ "$1" = "--reroll-serial" ] || [ ! -f "$SEED_FILE" ]; then
    echo "[*] Randomizing Samsung Serial Number (Seed Marker)..."
    YEAR_ARR="R T W X Y"
    MONTH_ARR="1 2 3 4 5 6 7 8 9 A B C"
    r_rand_year=$(( $(od -An -tu4 -N2 /dev/urandom 2>/dev/null | tr -d ' ' || echo 0) % 5 ))
    r_rand_month=$(( $(od -An -tu4 -N2 /dev/urandom 2>/dev/null | tr -d ' ' || echo 0) % 12 ))
    idx=0; r_year="R"
    for y in $YEAR_ARR; do [ $idx -eq $r_rand_year ] && r_year="$y" && break; idx=$((idx + 1)); done
    idx=0; r_month="1"
    for m in $MONTH_ARR; do [ $idx -eq $r_rand_month ] && r_month="$m" && break; idx=$((idx + 1)); done
    r_body=$(head -c 32 /dev/urandom 2>/dev/null | tr -dc '0-9ABCDEFGHJKLMNPQRSTUVWXYZ' | head -c 7)
    [ ${#r_body} -lt 7 ] && r_body="8K3N9P2"
    ACTIVE_SERIAL="R5${r_year}${r_month}${r_body}"
    mkdir -p /data/adb 2>/dev/null
    echo -n "$ACTIVE_SERIAL" > "$SEED_FILE" 2>/dev/null
    echo -n "$ACTIVE_SERIAL" > "$SEED_BACKUP" 2>/dev/null
    chmod 644 "$SEED_FILE" "$SEED_BACKUP" 2>/dev/null
    [ -d /efs/FactoryApp ] && echo -n "$ACTIVE_SERIAL" > /efs/FactoryApp/serial_no 2>/dev/null
    [ -d /mnt/vendor/efs/FactoryApp ] && echo -n "$ACTIVE_SERIAL" > /mnt/vendor/efs/FactoryApp/serial_no 2>/dev/null
else
    ACTIVE_SERIAL=$(cat "$SEED_FILE" 2>/dev/null | tr -d ' \r\n\t')
    [ -z "$ACTIVE_SERIAL" ] && ACTIVE_SERIAL=$(cat "$SEED_BACKUP" 2>/dev/null | tr -d ' \r\n\t')
    [ -z "$ACTIVE_SERIAL" ] && ACTIVE_SERIAL=$(cat /efs/FactoryApp/serial_no 2>/dev/null | tr -d ' \r\n\t')
    echo "[*] Retaining Persistent Samsung Serial (Seed Guard): $ACTIVE_SERIAL"
fi

# 1c. AP Serial & EM DID Virtualization & Boot Counter Alignment
if [ "$1" = "--new-serial" ] || [ "$1" = "--reroll-serial" ] || [ ! -f "/data/.ghost_ap_seed" ]; then
    echo "[*] Randomizing AP Serial & EM DID..."
    NEW_AP_RAW=$(od -An -tx8 -N6 /dev/urandom 2>/dev/null | tr -d ' ' | tr '[:lower:]' '[:upper:]')
    [ ${#NEW_AP_RAW} -lt 12 ] && NEW_AP_RAW="979BE420021A"
    NEW_AP_SERIAL="0x${NEW_AP_RAW}"
    NEW_EM_DID="20$(echo "$NEW_AP_RAW" | tr '[:upper:]' '[:lower:]')11"
    echo -n "$NEW_AP_SERIAL" > /data/.ghost_ap_seed 2>/dev/null
    echo -n "$NEW_AP_SERIAL" > /data/system/.ghost_ap_seed 2>/dev/null
    echo -n "$NEW_AP_SERIAL" > /efs/ghost_ap_seed.txt 2>/dev/null
    resetprop -n ro.boot.ap_serial "$NEW_AP_SERIAL" 2>/dev/null
    resetprop -n ro.boot.em.did "$NEW_EM_DID" 2>/dev/null
    resetprop -n ro.boot.bore_cnt 3 2>/dev/null
else
    ACTIVE_AP=$(cat /data/.ghost_ap_seed 2>/dev/null | tr -d ' \r\n\t')
    [ -z "$ACTIVE_AP" ] && ACTIVE_AP=$(cat /efs/ghost_ap_seed.txt 2>/dev/null | tr -d ' \r\n\t')
    if [ -n "$ACTIVE_AP" ]; then
        resetprop -n ro.boot.ap_serial "$ACTIVE_AP" 2>/dev/null
        RAW_HEX=$(echo "$ACTIVE_AP" | sed 's/0x//i' | tr '[:upper:]' '[:lower:]')
        resetprop -n ro.boot.em.did "20${RAW_HEX}11" 2>/dev/null
        resetprop -n ro.boot.bore_cnt 3 2>/dev/null
    fi
fi

# 2. Reset Android ID (SettingsProvider) & Per-App SSAID Matrix (Pillar 45)
echo "[*] Generating fresh Android ID & clearing per-app SSAID matrix..."
NEW_ANDROID_ID=$(xxd -p -l 8 /dev/urandom 2>/dev/null)
if [ -n "$NEW_ANDROID_ID" ]; then
    settings put secure android_id "$NEW_ANDROID_ID" 2>/dev/null
fi
rm -f /data/system/users/0/settings_ssaid.xml 2>/dev/null

# 3. Reset Google Services Framework (GSF ID) & GAID Ad Tracking (Pillar 45)
echo "[*] Purging Google Services Framework (GSF) database & ad tracking..."
rm -rf /data/data/com.google.android.gsf/databases/* 2>/dev/null
GMS_DATA="/data/data/com.google.android.gms"
if [ -d "$GMS_DATA" ]; then
    rm -f "$GMS_DATA/shared_prefs/adid_settings.xml" 2>/dev/null
    rm -f "$GMS_DATA/shared_prefs/advertising_id"*.xml 2>/dev/null
    rm -rf "$GMS_DATA/databases/checkin.db"* "$GMS_DATA/databases/gservices.db"* 2>/dev/null
fi

# 4. Cellular SIM ICCID & IMSI Simulation (Pillar 41)
echo "[*] Simulating realistic Cellular SIM (ICCID / IMSI / Operator)..."
CARRIER_IDX=$(( $(od -An -tu4 -N4 /dev/urandom | tr -d ' ') % 3 ))
case $CARRIER_IDX in
    0)
        CARRIER_NAME="Viettel"
        OPERATOR_NUM="45204"
        ICCID_PREFIX="898404"
        IMSI_PREFIX="45204"
        ;;
    1)
        CARRIER_NAME="Mobifone"
        OPERATOR_NUM="45201"
        ICCID_PREFIX="898401"
        IMSI_PREFIX="45201"
        ;;
    *)
        CARRIER_NAME="Vinaphone"
        OPERATOR_NUM="45202"
        ICCID_PREFIX="898402"
        IMSI_PREFIX="45202"
        ;;
esac

# Compute Luhn check digit for ICCID (POSIX/mksh compatible)
luhn_checksum() {
    local num=$1
    local sum=0
    local double=1
    local i d
    i=$(( ${#num} - 1 ))
    while [ $i -ge 0 ]; do
        d=${num:$i:1}
        if [ $double -eq 1 ]; then
            d=$(( d * 2 ))
            [ $d -gt 9 ] && d=$(( d - 9 ))
            double=0
        else
            double=1
        fi
        sum=$(( sum + d ))
        i=$(( i - 1 ))
    done
    echo $(( (10 - (sum % 10)) % 10 ))
}

# Generate exactly 13 random digits for ICCID body (6 prefix + 13 body = 19 digits + 1 check)
RAND_ICCID_BODY=$(tr -dc '0-9' < /dev/urandom | head -c 13)
ICCID_PAYLOAD="${ICCID_PREFIX}${RAND_ICCID_BODY}"
CHECK_DIGIT=$(luhn_checksum "$ICCID_PAYLOAD")
NEW_SIM_ICCID="${ICCID_PAYLOAD}${CHECK_DIGIT}"

# Generate exactly 10 random digits for IMSI body (5 prefix + 10 body = 15)
RAND_IMSI_BODY=$(tr -dc '0-9' < /dev/urandom | head -c 10)
NEW_SIM_IMSI="${IMSI_PREFIX}${RAND_IMSI_BODY}"

setprop ril.sim.iccid "$NEW_SIM_ICCID" 2>/dev/null
setprop ril.iccid.sim1 "$NEW_SIM_ICCID" 2>/dev/null
setprop gsm.sim.operator.numeric "$OPERATOR_NUM" 2>/dev/null
setprop gsm.operator.numeric "$OPERATOR_NUM" 2>/dev/null
setprop gsm.sim.operator.alpha "$CARRIER_NAME" 2>/dev/null
setprop gsm.operator.alpha "$CARRIER_NAME" 2>/dev/null
rm -rf /data/user_de/0/com.android.providers.telephony/databases/telephony.db* 2>/dev/null

# 5. Bluetooth Device Name & Network Hostname (Pillar 40)
echo "[*] Randomizing Bluetooth Device Name & DHCP Hostname..."
RAND_BT_IDX=$(( $(tr -dc '0-9' < /dev/urandom | head -c 2) % 8 ))
case $RAND_BT_IDX in
    0) NEW_BT_NAME="Galaxy S21 5G" ;;
    1) NEW_BT_NAME="Samsung Galaxy S21" ;;
    2) NEW_BT_NAME="SM-G991B" ;;
    3) NEW_BT_NAME="Galaxy S21" ;;
    4) NEW_BT_NAME="Galaxy Phone" ;;
    5) NEW_BT_NAME="Samsung Phone" ;;
    6) NEW_BT_NAME="Galaxy S21 Ultra" ;;
    *) NEW_BT_NAME="S21 5G" ;;
esac

settings put global bluetooth_name "$NEW_BT_NAME" 2>/dev/null
setprop net.bt.name "$NEW_BT_NAME" 2>/dev/null
setprop persist.sys.bluetooth.name "$NEW_BT_NAME" 2>/dev/null

NEW_HOSTNAME="android-$(xxd -p -l 8 /dev/urandom 2>/dev/null)"
setprop net.hostname "$NEW_HOSTNAME" 2>/dev/null

# 6. MediaStore Generation & Media Database Refresh (Pillar 42)
echo "[*] Refreshing MediaStore Generation counters..."
rm -f /data/data/com.android.providers.media.module/databases/external.db* 2>/dev/null
rm -f /data/data/com.android.providers.media/databases/external.db* 2>/dev/null

# 7. GPU Shader Cache Cleanup — targeted apps only (Pillar 44)
# Wiping ALL apps' code_cache is too aggressive (causes jank + fresh-device signal).
# Only purge for fingerprinting-relevant packages and WebView/Chrome.
echo "[*] Wiping targeted GPU Shader Binary caches..."
for CACHE_PKG in com.shopee.vn com.shopee.app com.android.webview com.google.android.webview com.android.chrome; do
    rm -rf "/data/user_de/0/$CACHE_PKG/code_cache" 2>/dev/null
    rm -rf "/data/user/0/$CACHE_PKG/code_cache" 2>/dev/null
done

# 8. Reset Boot Count to natural range [22..48]
RAND_BC=$(od -An -tu4 -N4 /dev/urandom | tr -d ' ')
NEW_BC=$(( (RAND_BC % 27) + 22 ))
settings put global boot_count "$NEW_BC" 2>/dev/null

# 9. Sanitize Environment Flags & Sync Location Provider (Pillars 35, 48)
echo "[*] Sanitizing mock location & aligning Location Providers..."
settings put secure mock_location 0 2>/dev/null
settings put secure location_mode 3 2>/dev/null
settings put secure location_providers_allowed "gps,network" 2>/dev/null

# 10. Sanitize Boot Reason, DSMS & Crash Dumps
echo "[*] Neutralizing DSMS telemetry & crash loop..."
setprop security.dsmsd.enable false 2>/dev/null
stop dsmsd 2>/dev/null
stop dsmsca 2>/dev/null

echo "[*] Resolving CSC carrier XML mapping..."
if [ ! -f /system/csc/customer.xml ] && [ -f /prism/etc/csc/customer.xml ]; then
    mkdir -p /system/csc 2>/dev/null
    mount -o bind /prism/etc/csc /system/csc 2>/dev/null || true
fi

echo "[*] Purging DropBox WTF, persistent properties & crash records..."
rm -rf /data/tombstones/* /data/system/dropbox/* 2>/dev/null
rm -rf /data/system/usagestats/* 2>/dev/null

# Ghost Kernel (Pillar 46): BatteryStats Timeline & File Timestamp Synchronization
if [ -f /data/system/batterystats.bin ]; then
    BTIME=$(awk '/^btime/{print $2}' /proc/stat)
    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        # Align batterystats file timestamp to the simulated boot epoch
        BT_FORMAT=$(date -d "@$BTIME" +%Y%m%d%H%M.%S 2>/dev/null || date +%Y%m%d%H%M.%S)
        touch -t "$BT_FORMAT" /data/system/batterystats*.bin 2>/dev/null
    fi
fi

setprop sys.boot.reason "reboot" 2>/dev/null
setprop sys.boot.reason.last "reboot" 2>/dev/null

RESETPROP=$(command -v resetprop 2>/dev/null)
if [ -z "$RESETPROP" ]; then
    [ -x /data/adb/ksu/bin/resetprop ] && RESETPROP="/data/adb/ksu/bin/resetprop"
    [ -x /data/adb/magisk/resetprop ] && RESETPROP="/data/adb/magisk/resetprop"
    [ -x /data/adb/ap/bin/resetprop ] && RESETPROP="/data/adb/ap/bin/resetprop"
fi

if [ -n "$RESETPROP" ]; then
    # Sanitize RescueParty and factory_reset logs (Pillar 49)
    $RESETPROP --delete sys.rescue_boot_count 2>/dev/null
    $RESETPROP --delete sys.rescue_boot_start 2>/dev/null
    $RESETPROP --delete sys.boot.reason 2>/dev/null
    $RESETPROP -n sys.boot.reason "reboot" 2>/dev/null
    $RESETPROP -n ro.boot.bootreason "reboot" 2>/dev/null

    # Synchronize Serial Number properties with Seed Guard
    if [ -n "$ACTIVE_SERIAL" ] && [ ${#ACTIVE_SERIAL} -eq 11 ]; then
        $RESETPROP -n ro.serialno "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -n ro.boot.serialno "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -n gsm.sn1 "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -n ril.serialnumber "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -p ro.serialno "$ACTIVE_SERIAL" 2>/dev/null
        $RESETPROP -p ro.boot.serialno "$ACTIVE_SERIAL" 2>/dev/null
    fi

    # Synchronize Verified Boot Hash & Key properties
    $RESETPROP -n ro.boot.vbmeta.digest "7207368a4caca12d62f0382e67932c38f78c6d0b3f9bd7f5967825461b4172c1" 2>/dev/null
    $RESETPROP -n ro.boot.boot_hash "7207368a4caca12d62f0382e67932c38f78c6d0b3f9bd7f5967825461b4172c1" 2>/dev/null
    $RESETPROP -n ro.boot.bootkey "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781" 2>/dev/null
    $RESETPROP -n ro.boot.verifiedbootkey "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781" 2>/dev/null
    $RESETPROP -n ro.boot.vbmeta.public_key_digest "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781" 2>/dev/null
    $RESETPROP -n ro.build.version.security_patch "2024-05-01" 2>/dev/null
    $RESETPROP -n ro.vendor.build.security_patch "2024-05-01" 2>/dev/null
    $RESETPROP -n gsm.operator.alpha "VIETTEL" 2>/dev/null
    $RESETPROP -n gsm.sim.operator.alpha "VIETTEL" 2>/dev/null
    $RESETPROP -n gsm.operator.numeric "45204" 2>/dev/null
    $RESETPROP -n gsm.sim.operator.numeric "45204" 2>/dev/null
    $RESETPROP -n gsm.operator.iso-country "vn" 2>/dev/null
    $RESETPROP -n gsm.network.type "LTE" 2>/dev/null

    BTIME=$(awk '/^btime/{print $2}' /proc/stat)
    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        ANCHOR=$((BTIME + 90))
        $RESETPROP -p persist.sys.boot.reason.history "reboot,$(( ANCHOR ))
reboot,$(( ANCHOR - 28800 ))" 2>/dev/null
        $RESETPROP -p persist.sys.boot.reason "" 2>/dev/null
    fi
fi

# 11. Target App Sandboxes Reset
TARGET_PACKAGES="com.shopee.vn com.shopee.app"
for PKG in $TARGET_PACKAGES; do
    PKG_DATA="/data/data/$PKG"
    if [ -d "$PKG_DATA" ]; then
        echo "[*] Sanitizing fingerprint artifacts for $PKG..."
        rm -rf "$PKG_DATA/files/web/dfdata" 2>/dev/null
        rm -rf "$PKG_DATA/shared_prefs/u0.xml" 2>/dev/null
        rm -rf "$PKG_DATA/shared_prefs/deviceId*.xml" 2>/dev/null
        rm -rf "$PKG_DATA/shared_prefs/fingerprint*.xml" 2>/dev/null
        rm -rf "$PKG_DATA/cache"/* 2>/dev/null
        rm -rf "$PKG_DATA/code_cache"/* 2>/dev/null
        am force-stop "$PKG" 2>/dev/null
    fi
done

# 12. Screen Lock Security Credential (PIN 1234)
echo "[*] Setting secure PIN lock credential..."
cmd lock_settings set-pin 1234 2>/dev/null || true

# 13. Display Full Unified 45-Pillar Profile State
SERIAL_NO="${ACTIVE_SERIAL:-$(cat /efs/FactoryApp/serial_no 2>/dev/null)}"
IMEI_NO=$(cat /proc/ghost_imei 2>/dev/null)
WIDEVINE_ID=$(grep widevine_device_id /proc/ghost_widevine 2>/dev/null | cut -d' ' -f2 | head -c 16)

# Read kernel state once (avoids 7x cat + race conditions)
GHOST_STATE=$(cat /proc/ghost_storage 2>/dev/null)
TCP_ISN=$(echo "$GHOST_STATE" | grep tcp_isn_offset | cut -d' ' -f2)
BATT_CYC=$(echo "$GHOST_STATE" | grep battery_cycle | cut -d' ' -f2)
UFS_SN=$(echo "$GHOST_STATE" | grep ufs_serial | cut -d' ' -f2)
SCSI_WWID=$(echo "$GHOST_STATE" | grep scsi_wwid | cut -d' ' -f2)
ACCEL_BIAS=$(echo "$GHOST_STATE" | grep sensor_bias | cut -d' ' -f2-)
GYRO_BIAS=$(echo "$GHOST_STATE" | grep gyro_bias | cut -d' ' -f2-)
BARO_DRIFT=$(echo "$GHOST_STATE" | grep baro_drift_hpa | cut -d' ' -f2)

echo ""
echo "-------------------------------------------------------------"
echo "  GHOST KERNEL 45-PILLAR UNIFIED IDENTITY PROFILE DETAILS    "
echo "-------------------------------------------------------------"
echo "  • Samsung Serial : $SERIAL_NO"
echo "  • Cellular IMEI  : $IMEI_NO"
echo "  • SIM Carrier    : $CARRIER_NAME ($OPERATOR_NUM)"
echo "  • SIM ICCID      : $NEW_SIM_ICCID"
echo "  • SIM IMSI       : $NEW_SIM_IMSI"
echo "  • Bluetooth Name : $NEW_BT_NAME"
echo "  • DHCP Hostname  : $NEW_HOSTNAME"
echo "  • Android ID     : $(settings get secure android_id 2>/dev/null)"
echo "  • UFS Serial     : $UFS_SN"
echo "  • SCSI WWID      : $SCSI_WWID"
echo "  • Widevine ID    : ${WIDEVINE_ID}..."
echo "  • Accel Bias     : $ACCEL_BIAS"
echo "  • Gyro Bias      : $GYRO_BIAS"
echo "  • Baro Drift     : ${BARO_DRIFT} hPa"
echo "  • TCP ISN Offset : $TCP_ISN"
echo "  • Battery Cycle  : $BATT_CYC"
echo "  • Boot Count     : $(settings get global boot_count 2>/dev/null)"
echo "  • Dev Mode       : $(settings get global development_settings_enabled 2>/dev/null)"
echo "  • ADB Debug      : $(settings get global adb_enabled 2>/dev/null)"
echo "  • Knox Status    : $(getprop ro.boot.warranty_bit) (Knox 0x0)"
echo "-------------------------------------------------------------"
echo "[✔] 50-Pillar Profile Reset Successfully Completed!"
echo "============================================================="
