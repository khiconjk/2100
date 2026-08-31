#!/system/bin/sh
if [ -f /proc/ghost_widevine_raw ]; then
    chmod 444 /proc/ghost_widevine_raw
    chmod 444 /proc/ghost_widevine
fi
