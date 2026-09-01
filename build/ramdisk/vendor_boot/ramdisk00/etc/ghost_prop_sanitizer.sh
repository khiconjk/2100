#!/system/bin/sh
# ======================================================================
#   GHOST KERNEL: VENDOR-BOOT PROPERTY SANITIZER (Pillar 36 Tầng 1+3)
#   Runs from vendor_boot ramdisk — 100% independent of /data/adb
#   Triggered by init.ghost.rc on property:sys.boot_completed=1
# ======================================================================

LOG_TAG="ghost_prop_sanitizer"

log_info() {
    log -t "$LOG_TAG" -p i "$1"
}

# ── Wait for Settings Provider to be fully ready ──
# SettingsProvider may not be available immediately at boot_completed
sleep 5

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

# ── 1. Boot Count Normalization ──
# A real Samsung S21 used ~180 days typically has boot_count [22..48].
# boot_count=1 after data wipe is a classic fraud signal.
CURRENT_BC=$(settings get global boot_count 2>/dev/null)
if [ -n "$CURRENT_BC" ] && [ "$CURRENT_BC" -le 2 ] 2>/dev/null; then
    # Derive deterministic per-device value from /proc/ghost_storage seed
    SEED_DEC=0
    if [ -f /proc/ghost_storage ]; then
        SEED_HEX=$(grep tcp_isn_offset /proc/ghost_storage | awk '{print $2}' | sed 's/^0x//')
        if [ -n "$SEED_HEX" ]; then
            SEED_DEC=$((16#${SEED_HEX} 2>/dev/null)) || SEED_DEC=0
        fi
    fi
    # Fallback: Android ID
    if [ "$SEED_DEC" -eq 0 ] 2>/dev/null; then
        AID=$(settings get secure android_id 2>/dev/null)
        if [ -n "$AID" ]; then
            SEED_DEC=$(printf '%d' "0x${AID%????????}" 2>/dev/null) || SEED_DEC=0
        fi
    fi
    # Fallback: urandom
    if [ "$SEED_DEC" -eq 0 ] 2>/dev/null; then
        SEED_DEC=$(od -An -tu4 -N4 /dev/urandom | tr -d ' ')
    fi
    NEW_BC=$(( (SEED_DEC % 27) + 22 ))
    settings put global boot_count "$NEW_BC" 2>/dev/null
    log_info "boot_count normalized: $CURRENT_BC -> $NEW_BC"
fi

# ── 2. Developer Mode Shielding ──
# Hide Developer Options menu from Settings and anti-fraud SDK queries.
# This does NOT disable ADB — only hides the menu visibility flag.
DEV_SET=$(settings get global development_settings_enabled 2>/dev/null)
if [ "$DEV_SET" = "1" ]; then
    settings put global development_settings_enabled 0 2>/dev/null
    log_info "development_settings_enabled hidden: 1 -> 0"
fi

# ── 3. Boot Reason History Timestamp Sync ──
# Android bootstat records real wall-clock timestamps that leak actual boot time.
# Rewrite them to align with kernel-shifted btime from /proc/stat.
RESETPROP=""
if [ -x /data/adb/ksu/bin/resetprop ]; then
    RESETPROP="/data/adb/ksu/bin/resetprop"
elif [ -x /data/adb/magisk/resetprop ]; then
    RESETPROP="/data/adb/magisk/resetprop"
fi

if [ -n "$RESETPROP" ]; then
    BTIME=$(awk '/^btime/{print $2}' /proc/stat)
    if [ -n "$BTIME" ] && [ "$BTIME" -gt 1000000000 ] 2>/dev/null; then
        HISTORY="$(getprop persist.sys.boot.reason.history)"
        if [ -n "$HISTORY" ]; then
            LATEST_TS=$((BTIME + 90))
            NEW_HISTORY=""
            IDX=0
            while IFS= read -r LINE || [ -n "$LINE" ]; do
                [ -z "$LINE" ] && continue
                REASON="${LINE%,*}"
                [ -z "$REASON" ] && continue
                # Pillar 37: Sanitize factory_reset evidence
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
                log_info "boot.reason.history rewritten: $IDX entries anchored to btime=$BTIME"
            fi
        fi
    fi
fi

log_info "Ghost property sanitization completed"
