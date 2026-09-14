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

# Serial properties stay aligned with Seed Guard / kernel epoch serial.
RESETPROP=$(command -v resetprop 2>/dev/null)
if [ -z "$RESETPROP" ]; then
    [ -x /data/adb/ksu/bin/resetprop ] && RESETPROP="/data/adb/ksu/bin/resetprop"
    [ -x /data/adb/magisk/resetprop ] && RESETPROP="/data/adb/magisk/resetprop"
    [ -x /data/adb/ap/bin/resetprop ] && RESETPROP="/data/adb/ap/bin/resetprop"
fi
if [ -n "$RESETPROP" ] && [ -n "$ACTIVE_SERIAL" ] && [ ${#ACTIVE_SERIAL} -eq 11 ]; then
    $RESETPROP -n ro.serialno "$ACTIVE_SERIAL" 2>/dev/null
    $RESETPROP -n ro.boot.serialno "$ACTIVE_SERIAL" 2>/dev/null
    $RESETPROP -n gsm.sn1 "$ACTIVE_SERIAL" 2>/dev/null
    $RESETPROP -n ril.serialnumber "$ACTIVE_SERIAL" 2>/dev/null
    $RESETPROP -p ro.serialno "$ACTIVE_SERIAL" 2>/dev/null
    $RESETPROP -p ro.boot.serialno "$ACTIVE_SERIAL" 2>/dev/null
fi

# Uptime companion: align batterystats file time to boot epoch.
if [ -f /data/system/batterystats.bin ]; then
    BTIME=$(awk '/^btime/{print $2}' /proc/stat)
    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        BT_FORMAT=$(date -d "@$BTIME" +%Y%m%d%H%M.%S 2>/dev/null || date +%Y%m%d%H%M.%S)
        touch -t "$BT_FORMAT" /data/system/batterystats*.bin 2>/dev/null
    fi
fi

echo ""
echo "-------------------------------------------------------------"
echo "  GHOST RESET: serial / AP serial / EM DID only"
echo "  Extra identity wipes are OFF (see GHOST_RESET_EXTRA_RESTORE.md)"
echo "-------------------------------------------------------------"
echo "  Samsung Serial : ${ACTIVE_SERIAL:-unknown}"
echo "-------------------------------------------------------------"
echo "[*] Serial-only reset completed"
echo "============================================================="
