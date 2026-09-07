#!/system/bin/sh
# ==============================================================================
# GHOST KERNEL - ONE-TOUCH PROFILE RANDOMIZER (Android /system/bin/sh compatible)
# Generates authentic Samsung S21 (SM-G991B) hardware profile with valid Luhn checksums
# ==============================================================================

CONF_PATH="/data/adb/ghost.conf"
[ -d "/efs" ] && [ -w "/efs" ] && CONF_PATH_ALT="/efs/ghost.conf"

echo "============================================================="
echo "       GHOST KERNEL DYNAMIC PROFILE GENERATOR                "
echo "============================================================="
echo "[*] Randomizing authentic Samsung S21 hardware profile..."

# 1. SAMSUNG SERIAL NUMBER GENERATION (11 chars: R5 + Year + Month + 7 alphanumeric)
# Year: R=2021, T=2022, W=2023, X=2024, Y=2025 | Month: 1-9, A, B, C
YEAR_CODES="R T W X Y"
MONTH_CODES="1 2 3 4 5 6 7 8 9 A B C"

YEAR_ARR=($YEAR_CODES)
MONTH_ARR=($MONTH_CODES)
R_YEAR=${YEAR_ARR[$(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 5 ))]}
R_MONTH=${MONTH_ARR[$(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 12 ))]}

R_BODY=$(head -c 16 /dev/urandom | tr -dc '0-9ABCDEFGHJKLMNPQRSTUVWXYZ' | head -c 7)
[ ${#R_BODY} -lt 7 ] && R_BODY="8K3N9P2"
SERIALNO="R5${R_YEAR}${R_MONTH}${R_BODY}"

# 2. DUAL SIM IMEI GENERATION WITH MATHEMATICALLY CORRECT LUHN CHECKSUM (Mod 10)
TACS="35971387 35966984 35918923 35906784 35895793 35844692 35833295 35814433 35787240 35471978"
TAC_ARR=($TACS)
NUM_TACS=10

TAC1_IDX=$(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % NUM_TACS ))
TAC2_IDX=$(( (TAC1_IDX + 1 + ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % (NUM_TACS - 1))) % NUM_TACS ))

TAC1=${TAC_ARR[$TAC1_IDX]}
TAC2=${TAC_ARR[$TAC2_IDX]}

# Standard Luhn Checksum Algorithm
calc_luhn() {
    num="$1"
    sum=0
    double=1
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
        i=$(( $i - 1 ))
    done
    echo $(( (10 - (sum % 10)) % 10 ))
}

SNR1=$(head -c 16 /dev/urandom | tr -dc '0-9' | head -c 6)
[ ${#SNR1} -lt 6 ] && SNR1="102938"
IMEI1_BODY="${TAC1}${SNR1}"
CHECK1=$(calc_luhn "$IMEI1_BODY")
IMEI1="${IMEI1_BODY}${CHECK1}"

SNR2=$(head -c 16 /dev/urandom | tr -dc '0-9' | head -c 6)
[ ${#SNR2} -lt 6 ] && SNR2="102939"
IMEI2_BODY="${TAC2}${SNR2}"
CHECK2=$(calc_luhn "$IMEI2_BODY")
IMEI2="${IMEI2_BODY}${CHECK2}"

# 3. SAMSUNG AUTHENTIC WI-FI & BLUETOOTH MAC
OUIS="A4:75:B9 00:12:FB 24:4B:FE 8C:77:12 5C:E9:1E E8:50:8B 30:CD:A7 F4:7B:5E"
OUI_ARR=($OUIS)
OUI_IDX=$(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 8 ))
SELECTED_OUI=${OUI_ARR[$OUI_IDX]}

B4=$(printf "%02X" $(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 256 )))
B5=$(printf "%02X" $(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 256 )))
B6_NUM=$(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 254 ))
B6=$(printf "%02X" $B6_NUM)
B6_BT=$(printf "%02X" $(( B6_NUM + 1 )))

WIFI_MAC="${SELECTED_OUI}:${B4}:${B5}:${B6}"
BT_MAC="${SELECTED_OUI}:${B4}:${B5}:${B6_BT}"

# 4. UFS 3.1 STORAGE IDENTIFIERS
UFS_MODELS="KLUDG8UHDB-C2D1 KLUEG8UHDB-C2D1 KLUCG4J1ED-B0C1 HN8T05BZGKX015 KM5V7001DM-B621"
UFS_ARR=($UFS_MODELS)
UFS_IDX=$(( $(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 5 ))
UFS_MODEL=${UFS_ARR[$UFS_IDX]}
UFS_SERIAL=$(printf "0x%08x" $(( $(od -An -tu4 -N4 /dev/urandom | tr -d ' ') )))

# 5. ENVIRONMENT & SENSOR TELEMETRY METRICS
UPTIME_DAYS=$(( ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 38) + 4 ))
BOOT_COUNT=$(( ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 65) + 18 ))
BATTERY_CYCLE=$(( ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 280) + 45 ))
BATTERY_HEALTH=$(( 100 - (BATTERY_CYCLE / 40) ))
[ $BATTERY_HEALTH -lt 85 ] && BATTERY_HEALTH=85
[ $BATTERY_HEALTH -gt 100 ] && BATTERY_HEALTH=98

BIAS_X=$(( ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 41) - 20 ))
BIAS_Y=$(( ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 41) - 20 ))
BIAS_Z=$(( ($(od -An -tu4 -N2 /dev/urandom | tr -d ' ') % 41) - 20 ))
SENSOR_BIAS="${BIAS_X},${BIAS_Y},${BIAS_Z}"

TCP_ISN=$(printf "0x%08x" $(( $(od -An -tu4 -N4 /dev/urandom | tr -d ' ') )))

# 6. WRITE CONFIG FILE
cat << CONF_EOF > "$CONF_PATH"
# ==============================================================================
# GHOST KERNEL 100% PURE KERNEL CONFIGURATION FILE
# ==============================================================================

[DEVICE]
model = SM-G991B
product = o1sxeea
device = o1s
manufacturer = samsung
brand = samsung
soc_machine = Exynos
soc_family = samsung

[FINGERPRINT]
build_fingerprint = samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXS3BULC:user/release-keys
build_desc = o1sxeea-user 12 SP1A.210812.016 SM-G991BXXS3BULC release-keys
build_id = SP1A.210812.016
security_patch = 2022-01-01

[HARDWARE_IDS]
serialno = $SERIALNO
imei = $IMEI1
imei2 = $IMEI2
wifi_mac = $WIFI_MAC
bt_mac = $BT_MAC
ufs_serial = $UFS_SERIAL
ufs_model = $UFS_MODEL

[ENVIRONMENT_METRICS]
uptime_days = $UPTIME_DAYS
boot_count = $BOOT_COUNT
battery_cycle = $BATTERY_CYCLE
battery_health = $BATTERY_HEALTH
sensor_bias = $SENSOR_BIAS
tcp_isn_offset = $TCP_ISN
CONF_EOF

chmod 644 "$CONF_PATH"
if [ -n "$CONF_PATH_ALT" ]; then
    cp -f "$CONF_PATH" "$CONF_PATH_ALT" 2>/dev/null
    chmod 644 "$CONF_PATH_ALT" 2>/dev/null
fi

# Sync Seed Guard files and EFS
mkdir -p /data/adb 2>/dev/null
echo -n "$SERIALNO" > /data/adb/.ghost_serial_seed 2>/dev/null
echo -n "$SERIALNO" > /data/.ghost_serial_seed 2>/dev/null
chmod 644 /data/adb/.ghost_serial_seed /data/.ghost_serial_seed 2>/dev/null
[ -d /efs/FactoryApp ] && echo -n "$SERIALNO" > /efs/FactoryApp/serial_no 2>/dev/null
[ -d /mnt/vendor/efs/FactoryApp ] && echo -n "$SERIALNO" > /mnt/vendor/efs/FactoryApp/serial_no 2>/dev/null

# Sync Props via resetprop
RESETPROP=$(command -v resetprop 2>/dev/null)
[ -z "$RESETPROP" ] && [ -x /data/adb/ksu/bin/resetprop ] && RESETPROP="/data/adb/ksu/bin/resetprop"
if [ -n "$RESETPROP" ]; then
    $RESETPROP -n ro.serialno "$SERIALNO" 2>/dev/null
    $RESETPROP -n ro.boot.serialno "$SERIALNO" 2>/dev/null
    $RESETPROP -n gsm.sn1 "$SERIALNO" 2>/dev/null
    $RESETPROP -n ril.serialnumber "$SERIALNO" 2>/dev/null
    $RESETPROP -p ro.serialno "$SERIALNO" 2>/dev/null
    $RESETPROP -p ro.boot.serialno "$SERIALNO" 2>/dev/null
fi

# 7. TRIGGER KERNEL HOT RELOAD
if [ -e /proc/ghost_reload ]; then
    echo "reload" > /proc/ghost_reload 2>/dev/null || echo 1 > /proc/ghost_reload 2>/dev/null
fi

echo "[✔] New Ghost Hardware Profile generated successfully at $CONF_PATH!"
echo "-------------------------------------------------------------"
echo "  • Serial Number  : $SERIALNO"
echo "  • IMEI 1 (Slot 1): $IMEI1 (Valid Luhn: $CHECK1)"
echo "  • IMEI 2 (Slot 2): $IMEI2 (Valid Luhn: $CHECK2)"
echo "  • Wi-Fi MAC      : $WIFI_MAC (Samsung OUI: $SELECTED_OUI)"
echo "  • Bluetooth MAC  : $BT_MAC"
echo "  • UFS Chip Model : $UFS_MODEL (SN: $UFS_SERIAL)"
echo "  • Battery Stats  : $BATTERY_CYCLE cycles (${BATTERY_HEALTH}% health)"
echo "  • Uptime Metrics : $UPTIME_DAYS days ($BOOT_COUNT boots)"
echo "-------------------------------------------------------------"
if [ -e /proc/ghost_config ]; then
    echo "[*] Kernel Hot-Reload Verification:"
    cat /proc/ghost_config | grep -E "Serial Number|Cellular IMEI|Wi-Fi MAC|Bluetooth Address|UFS Serial"
fi
echo "============================================================="
