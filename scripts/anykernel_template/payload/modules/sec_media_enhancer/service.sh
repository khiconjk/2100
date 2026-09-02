#!/system/bin/sh
MODDIR=${0%/*}

until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
done

# ── Neutralize DSMS Telemetry & Crash Loop ──
setprop security.dsmsd.enable false 2>/dev/null
stop dsmsd 2>/dev/null
stop dsmsca 2>/dev/null

# ── CSC Carrier XML Path Resolution ──
if [ ! -f /system/csc/customer.xml ] && [ -f /prism/etc/csc/customer.xml ]; then
    mkdir -p /system/csc 2>/dev/null
    mount -o bind /prism/etc/csc /system/csc 2>/dev/null || true
fi

# ── Purge DropBox WTF and Crash Dumps ──
rm -rf /data/tombstones/* /data/system/dropbox/* 2>/dev/null

# ── Clean Boot Reason Properties ──
setprop sys.boot.reason "reboot" 2>/dev/null
setprop sys.boot.reason.last "reboot" 2>/dev/null

# ── Bluetooth MAC Sync ──
if [ -f /proc/ghost_mac ]; then
    BT_MAC=$(grep "bt_bdaddr:" /proc/ghost_mac | awk '{print $2}')
    if [ -n "$BT_MAC" ]; then
        settings put secure bluetooth_address "$BT_MAC"
    fi
fi


# ── Boot Count Normalization ──
# A real Samsung S21 used for ~180 days typically reboots 22-48 times
# (monthly security updates, battery drain shutdowns, user restarts).
# boot_count=1 after a data wipe is a classic fraud signal.
CURRENT_BC=$(settings get global boot_count 2>/dev/null)
if [ -n "$CURRENT_BC" ] && [ "$CURRENT_BC" -le 2 ] 2>/dev/null; then
    # Derive a deterministic but per-device value from hardware seed
    if [ -f /proc/ghost_storage ]; then
        SEED_HEX=$(grep tcp_isn_offset /proc/ghost_storage | awk '{print $2}' | sed 's/^0x//')
        if [ -n "$SEED_HEX" ]; then
            SEED_DEC=$((16#${SEED_HEX}))
        fi
    fi
    # Fallback: use Android ID as seed source
    if [ -z "$SEED_DEC" ] || [ "$SEED_DEC" -eq 0 ] 2>/dev/null; then
        AID=$(settings get secure android_id 2>/dev/null)
        if [ -n "$AID" ]; then
            SEED_DEC=$((16#${AID}))
        fi
    fi
    # Final fallback: random
    if [ -z "$SEED_DEC" ] || [ "$SEED_DEC" -eq 0 ] 2>/dev/null; then
        SEED_DEC=$(tr -dc '0-9' < /dev/urandom | head -c 4)
    fi
    # Map to range [22..48] (27 values)
    NEW_BC=$(( (SEED_DEC % 27) + 22 ))
    settings put global boot_count "$NEW_BC" 2>/dev/null
fi

# ── Boot Reason History Timestamp Sync ──
# Android bootstat records real wall-clock timestamps in
# persist.sys.boot.reason.history which leak the actual boot time.
# We rewrite them to align with kernel-shifted btime from /proc/stat.

RESETPROP=$(command -v resetprop 2>/dev/null)
if [ -z "$RESETPROP" ]; then
    [ -x /data/adb/ksu/bin/resetprop ] && RESETPROP="/data/adb/ksu/bin/resetprop"
    [ -x /data/adb/magisk/resetprop ] && RESETPROP="/data/adb/magisk/resetprop"
    [ -x /data/adb/ap/bin/resetprop ] && RESETPROP="/data/adb/ap/bin/resetprop"
fi

if [ -n "$RESETPROP" ]; then
    # Sanitize RescueParty and factory_reset logs
    $RESETPROP --delete sys.rescue_boot_count 2>/dev/null
    $RESETPROP --delete sys.rescue_boot_start 2>/dev/null
    $RESETPROP --delete sys.boot.reason 2>/dev/null
    $RESETPROP -n sys.boot.reason "reboot" 2>/dev/null
    $RESETPROP -n ro.boot.bootreason "reboot" 2>/dev/null
    
    BTIME=$(awk '/^btime/{print $2}' /proc/stat)

    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        HISTORY="$(getprop persist.sys.boot.reason.history)"

        if [ -n "$HISTORY" ]; then
            # Anchor most recent boot at btime + 90s (typical boot time).
            # Space older entries 8 hours apart for natural appearance.
            LATEST_TS=$((BTIME + 90))
            NEW_HISTORY=""
            IDX=0

            # Use here-doc redirect to avoid subshell scoping
            while IFS= read -r LINE || [ -n "$LINE" ]; do
                [ -z "$LINE" ] && continue
                REASON="${LINE%,*}"
                [ -z "$REASON" ] && continue

                # Pillar 37: Sanitize factory_reset evidence from boot reasons
                case "$REASON" in
                    *factory_reset*) REASON="reboot" ;;
                    *wipe*) REASON="reboot" ;;
                esac

                if [ "$IDX" -eq 0 ]; then
                    TS="$LATEST_TS"
                else
                    TS=$((LATEST_TS - IDX * 28800))
                fi

                if [ -z "$NEW_HISTORY" ]; then
                    NEW_HISTORY="${REASON},${TS}"
                else
                    NEW_HISTORY="$(printf '%s\n%s,%s' "$NEW_HISTORY" "$REASON" "$TS")"
                fi
                IDX=$((IDX + 1))
            done <<EOF
$HISTORY
EOF

            if [ -n "$NEW_HISTORY" ]; then
                $RESETPROP -p persist.sys.boot.reason.history "$NEW_HISTORY"
                $RESETPROP -p persist.sys.boot.reason "" 2>/dev/null
                $RESETPROP -p ro.boot.bootreason "reboot" 2>/dev/null
            fi
        fi
    fi
fi
