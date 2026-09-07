#!/system/bin/sh
echo "=== 1. KERNEL AND KSU ==="
uname -a
su -v
id
ksud susfs version
ksud susfs variant
ksud susfs features

echo "=== 2. HARDWARE INTEGRITY AND KNOX ==="
echo "warranty_bit: $(getprop ro.boot.warranty_bit)"
echo "flash.locked: $(getprop ro.boot.flash.locked)"
echo "verifiedbootstate: $(getprop ro.boot.verifiedbootstate)"
echo "veritymode: $(getprop ro.boot.veritymode)"
echo "boot.serialno: $(getprop ro.boot.serialno)"
echo "ro.serialno: $(getprop ro.serialno)"
echo "security_patch: $(getprop ro.build.version.security_patch)"
echo "vendor_security_patch: $(getprop ro.vendor.build.security_patch)"

echo "=== 3. DEVICETREE HARDENING ==="
cat /proc/device-tree/chosen/bootargs | tr '\0' '\n' | grep -E '(androidboot|sec_debug|buildvariant)' | head -n 10

echo "=== 4. TELECOM, EFS & HARDWARE STORAGE SPOOFING ==="
echo "ghost_imei: $(cat /proc/ghost_imei 2>/dev/null)"
echo "efs_serial_root: $(cat /efs/FactoryApp/serial_no 2>/dev/null)"
echo "ufs_vendor: $(cat /sys/block/sda/device/vendor 2>/dev/null)"
echo "ufs_model: $(cat /sys/block/sda/device/model 2>/dev/null)"
echo "ufs_rev: $(cat /sys/block/sda/device/rev 2>/dev/null)"
echo "batt_type: $(cat /sys/class/power_supply/battery/batt_type 2>/dev/null)"
echo "radiostate: $(getprop ril.radiostate)"
echo "baseband: $(getprop gsm.version.baseband)"

echo "=== 5. THERMAL DYNAMIC ENTROPY ==="
for z in 0 1 2 3 4 5 6; do
  echo "Zone $z ($(cat /sys/class/thermal/thermal_zone$z/type 2>/dev/null)): $(cat /sys/class/thermal/thermal_zone$z/temp 2>/dev/null) mC"
done
echo "Battery Temp: $(cat /sys/class/power_supply/battery/temp 2>/dev/null)"

echo "=== 6. PROCFS AND MAPS CLOAKING TEST (UID 10000) ==="
su 10000 -c 'grep -iE "(ksu|zygisk|magisk|susfs)" /proc/self/maps' || echo "Maps clean for UID 10000 (No KSU/Zygisk leaked)"
su 10000 -c 'grep -iE "(ksu|zygisk|magisk|susfs)" /proc/self/mountinfo' || echo "Mountinfo clean for UID 10000"
su 10000 -c 'grep -iE "(ksu|zygisk|magisk)" /proc/net/unix' || echo "Unix sockets clean for UID 10000"
echo "efs_serial_UID10000: $(su 10000 -c 'cat /efs/FactoryApp/serial_no')"

echo "=== 7. MODULES AND DEFENSE SUITE ==="
ls -la /data/adb/modules
ls -la /data/adb/tricky_store
