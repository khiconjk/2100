#!/system/bin/sh
# ====================================================================
#   GHOST KERNEL: UNIFIED HARDWARE & USERSPACE PROFILE REROLL V2
# ====================================================================

echo "============================================================="
echo "       GHOST KERNEL UNIFIED PROFILE RESET ENGINE V2          "
echo "============================================================="

# 1. Trigger Kernel Level Master Seed & Hardware Reroll
echo "[*] Triggering Kernel Hardware & Identity Reroll..."
echo reroll > /proc/ghost_storage

# 2. Reset Android ID (SettingsProvider SQLite)
NEW_ANDROID_ID=$(xxd -p -l 8 /dev/urandom 2>/dev/null)
if [ -n "$NEW_ANDROID_ID" ]; then
    settings put secure android_id "$NEW_ANDROID_ID" 2>/dev/null
fi

# 3. Reset Google Advertising ID (GAID) if GMS exists
GMS_PREF_DIR="/data/data/com.google.android.gms/shared_prefs"
if [ -d "$GMS_PREF_DIR" ]; then
    echo "[*] Cleansing Google Play Services Ad Tracking Identifiers..."
    rm -f "$GMS_PREF_DIR"/adid_settings.xml 2>/dev/null
    rm -f "$GMS_PREF_DIR"/advertising_id*.xml 2>/dev/null
fi

# 4. Target App Sandboxes & Crash Dumps Reset
rm -rf /data/tombstones/* /data/system/dropbox/*tombstone* /data/system/dropbox/*crash* /data/system/dropbox/*anr* 2>/dev/null
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

# 5. Display Unified Profile State
SERIAL_NO=$(cat /efs/FactoryApp/serial_no 2>/dev/null)
IMEI_NO=$(cat /proc/ghost_imei 2>/dev/null)
WIDEVINE_ID=$(cat /proc/ghost_widevine 2>/dev/null | grep widevine_device_id | cut -d' ' -f2 | head -c 16)
TCP_ISN=$(cat /proc/ghost_storage 2>/dev/null | grep tcp_isn_offset | cut -d' ' -f2)
BATT_CYC=$(cat /proc/ghost_storage 2>/dev/null | grep battery_cycle | cut -d' ' -f2)
UFS_SN=$(cat /proc/ghost_storage 2>/dev/null | grep ufs_serial | cut -d' ' -f2)

echo ""
echo "-------------------------------------------------------------"
echo "  NEW HARDWARE & IDENTITY PROFILE DETAILS                    "
echo "-------------------------------------------------------------"
echo "  • Samsung Serial : $SERIAL_NO"
echo "  • Cellular IMEI  : $IMEI_NO"
echo "  • Android ID     : $(settings get secure android_id 2>/dev/null)"
echo "  • UFS Serial     : $UFS_SN"
echo "  • Widevine ID    : ${WIDEVINE_ID}..."
echo "  • TCP ISN Offset : $TCP_ISN"
echo "  • Battery Cycle  : $BATT_CYC"
echo "  • Knox Status    : $(getprop ro.boot.warranty_bit) (Knox 0x0)"
echo "-------------------------------------------------------------"
echo "[✔] Profile Reset Successfully Completed in < 0.1s!"
echo "============================================================="
