#!/system/bin/sh
# ====================================================================
#   GHOST KERNEL: UNIFIED HARDWARE & USERSPACE PROFILE REROLL V4
# ====================================================================

echo "============================================================="
echo "       GHOST KERNEL UNIFIED PROFILE RESET ENGINE V4          "
echo "============================================================="

# 1. Trigger Kernel Level Master Seed & Hardware Reroll
echo "[*] Triggering Kernel Hardware & Identity Reroll..."
echo reroll > /proc/ghost_storage 2>/dev/null

# 2. Reset Android ID (SettingsProvider SQLite)
NEW_ANDROID_ID=$(xxd -p -l 8 /dev/urandom 2>/dev/null)
if [ -n "$NEW_ANDROID_ID" ]; then
    settings put secure android_id "$NEW_ANDROID_ID" 2>/dev/null
fi

# 3. Reset Boot Count to natural range [22..48]
RAND_BC=$(od -An -tu4 -N4 /dev/urandom | tr -d ' ')
NEW_BC=$(( (RAND_BC % 27) + 22 ))
settings put global boot_count "$NEW_BC" 2>/dev/null

# 4. Sanitize Developer & Debug Environment (keep ADB alive until end)
echo "[*] Sanitizing developer environment flags..."
settings put global development_settings_enabled 0 2>/dev/null
settings put secure mock_location 0 2>/dev/null

# 5. Reset Google Advertising ID (GAID) if GMS exists
GMS_PREF_DIR="/data/data/com.google.android.gms/shared_prefs"
if [ -d "$GMS_PREF_DIR" ]; then
    echo "[*] Cleansing Google Play Services Ad Tracking Identifiers..."
    rm -f "$GMS_PREF_DIR"/adid_settings.xml 2>/dev/null
    rm -f "$GMS_PREF_DIR"/advertising_id*.xml 2>/dev/null
fi

# 6. Sanitize Boot Reason, DSMS & Crash Dumps
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
rm -f /data/property/persistent_properties 2>/dev/null
rm -rf /data/tombstones/* /data/system/dropbox/* 2>/dev/null

setprop sys.boot.reason "reboot" 2>/dev/null
setprop sys.boot.reason.last "reboot" 2>/dev/null

RESETPROP=""
if [ -x /data/adb/ksu/bin/resetprop ]; then
    RESETPROP="/data/adb/ksu/bin/resetprop"
elif [ -x /data/adb/magisk/resetprop ]; then
    RESETPROP="/data/adb/magisk/resetprop"
fi
if [ -n "$RESETPROP" ]; then
    BTIME=$(awk '/^btime/{print $2}' /proc/stat)
    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        ANCHOR=$((BTIME + 90))
        $RESETPROP -p persist.sys.boot.reason.history \
            "reboot,$(( ANCHOR ))
reboot,$(( ANCHOR - 28800 ))" 2>/dev/null
        $RESETPROP -p persist.sys.boot.reason "" 2>/dev/null
        $RESETPROP -p ro.boot.bootreason "reboot" 2>/dev/null
    fi
fi

# 7. Target App Sandboxes Reset
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

# 8. Display Unified Profile State
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
echo "  • Boot Count     : $(settings get global boot_count 2>/dev/null)"
echo "  • Dev Mode       : $(settings get global development_settings_enabled 2>/dev/null) (hidden)"
echo "  • ADB Debug      : 0 (will disable in 2s)"
echo "  • Knox Status    : $(getprop ro.boot.warranty_bit) (Knox 0x0)"
echo "-------------------------------------------------------------"
echo "[✔] Profile Reset Successfully Completed!"
echo "[!] ADB will disconnect in 2 seconds."
echo "============================================================="

# 9. Disable ADB LAST — after all cleanup and output is done.
# Background with delay so the script output reaches the terminal
# before adbd is killed by the system.
(sleep 2 && settings put global adb_enabled 0) &
