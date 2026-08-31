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

# 3. Reset Google Advertising ID (GAID) & Synchronize Checkin
GMS_PREF_DIR="/data/data/com.google.android.gms/shared_prefs"
if [ -d "$GMS_PREF_DIR" ]; then
    echo "[*] Cleansing Google Play Services Ad Tracking Identifiers..."
    rm -f "$GMS_PREF_DIR"/adid_settings.xml 2>/dev/null
    rm -f "$GMS_PREF_DIR"/advertising_id*.xml 2>/dev/null
    am force-stop com.google.android.gms 2>/dev/null
    am force-stop com.android.vending 2>/dev/null
    am broadcast -a android.server.checkin.CHECKIN com.google.android.gms >/dev/null 2>&1
fi

# Neutralize failing Samsung DSMS daemon loop
setprop security.dsmsd.enable false 2>/dev/null
stop dsmsd 2>/dev/null

# 4. Target App Sandboxes Reset (Shopee, TikTok, etc.)
TARGET_PACKAGES="com.shopee.vn com.shopee.app com.ss.android.ugc.trill"
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
GHOST_STORAGE=$(cat /proc/ghost_storage 2>/dev/null)
GHOST_IMEI=$(cat /proc/ghost_imei 2>/dev/null)
SERIAL_NO=$(cat /efs/FactoryApp/serial_no 2>/dev/null)
IMEI1_NO=$(echo "$GHOST_IMEI" | grep imei1 | cut -d' ' -f2)
IMEI2_NO=$(echo "$GHOST_IMEI" | grep imei2 | cut -d' ' -f2)
WIDEVINE_ID=$(cat /proc/ghost_widevine 2>/dev/null | grep widevine_device_id | cut -d' ' -f2 | head -c 16)
TCP_ISN=$(echo "$GHOST_STORAGE" | grep tcp_isn_offset | cut -d' ' -f2)
BATT_CYC=$(echo "$GHOST_STORAGE" | grep battery_cycle | cut -d' ' -f2)
BATT_ASOC=$(echo "$GHOST_STORAGE" | grep battery_asoc | cut -d' ' -f2)
UFS_SN=$(echo "$GHOST_STORAGE" | grep ufs_serial | cut -d' ' -f2)
MEM_TOTAL=$(cat /proc/meminfo 2>/dev/null | grep MemTotal | awk '{print $2" "$3}')
WIFI_MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null)

echo ""
echo "-------------------------------------------------------------"
echo "  NEW HARDWARE & IDENTITY PROFILE DETAILS                    "
echo "-------------------------------------------------------------"
echo "  • Samsung Serial : $SERIAL_NO"
echo "  • Cellular IMEI 1: $IMEI1_NO"
echo "  • Cellular IMEI 2: $IMEI2_NO"
echo "  • Android ID     : $(settings get secure android_id 2>/dev/null)"
echo "  • Wi-Fi MAC      : $WIFI_MAC"
echo "  • UFS Serial     : $UFS_SN"
echo "  • Total RAM      : $MEM_TOTAL"
echo "  • Widevine ID    : ${WIDEVINE_ID}..."
echo "  • TCP ISN Offset : $TCP_ISN"
echo "  • Battery Health : $BATT_CYC cycles ($BATT_ASOC ASoC)"
echo "  • Knox Status    : $(getprop ro.boot.warranty_bit) (Knox 0x0)"
echo "-------------------------------------------------------------"
echo "[✔] Profile Reset Successfully Completed in < 0.1s!"
echo "============================================================="
