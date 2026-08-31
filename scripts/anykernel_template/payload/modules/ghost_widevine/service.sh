#!/system/bin/sh
MODDIR=${0%/*}

until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
done

if [ -f /proc/ghost_mac ]; then
    BT_MAC=$(grep "bt_bdaddr:" /proc/ghost_mac | awk '{print $2}')
    if [ -n "$BT_MAC" ]; then
        settings put secure bluetooth_address "$BT_MAC"
    fi
fi

# Neutralize failing Samsung DSMS daemon loop to stop logcat spam and battery drain
setprop security.dsmsd.enable false
stop dsmsd 2>/dev/null
