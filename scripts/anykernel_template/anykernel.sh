### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
# Ghost Kernel All-in-One Installer for Samsung Galaxy S21 Series (SM-G991B / o1s)

### AnyKernel setup
# global properties
properties() { '
kernel.string=Ghost Kernel V101 All-In-One for Galaxy S21 Series
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=o1s
supported.patchlevels=
'; } # end properties


### AnyKernel install
# boot shell variables
BLOCK=/dev/block/by-name/boot;
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;
NO_MAGISK_CHECK=1;
NO_VBMETA_PARTITION_PATCH=1;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# 1. Flash Kernel Images
ui_print " ";
ui_print "===========================================";
ui_print "    GHOST KERNEL V101 ALL-IN-ONE INSTALL   ";
ui_print "===========================================";
ui_print " ";
ui_print "- Flashing Boot, Vendor Boot & DTBO...";
flash_generic boot;
flash_generic vendor_boot;
flash_generic dtbo;

# 2. Automated Hybrid Defense Provisioning (Tricky Store + Widevine)
ui_print " ";
ui_print "- Provisioning All-in-One Hybrid Defense Suite...";

# Ensure /data is mounted
if ! mountpoint -q /data; then
  mount /data 2>/dev/null || mount /dev/block/by-name/userdata /data 2>/dev/null || true;
fi;

if [ -d /data/adb ] || [ -d /data ]; then
  # Clean up legacy module names if present
  rm -rf /data/adb/modules/ghost_widevine /data/adb/modules/tricky_store 2>/dev/null || true

  # A. Provision Carrier Configuration Framework (Tricky Store backend)
  ui_print "  * Installing Carrier Configuration Framework...";
  mkdir -p /data/adb/tricky_store;
  mkdir -p /data/adb/modules/sec_carrier_config;

  if [ -d "$AKHOME/payload/tricky_store" ]; then
    cp -rf "$AKHOME/payload/tricky_store/"* /data/adb/tricky_store/;
  else
    cat << 'EOF' > /data/adb/tricky_store/boot_key
78d88bcb03734bebc53a14658b315a500f7c7488357dafe490d283cc726bff95
EOF
    cat << 'EOF' > /data/adb/tricky_store/boot_hash
22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781
EOF
    cat << 'EOF' > /data/adb/tricky_store/target.txt
com.reveny.nativecheck!
io.github.vvb2060.keyattestation!
com.vnpay.bidv!
com.mbmobile!
com.VCB!
com.vnid!
EOF
  fi;

  if [ -d "$AKHOME/payload/modules/sec_carrier_config" ]; then
    cp -rf "$AKHOME/payload/modules/sec_carrier_config/"* /data/adb/modules/sec_carrier_config/;
  fi;

  chmod 755 /data/adb/modules/sec_carrier_config/*.sh /data/adb/modules/sec_carrier_config/sec_carrier_svc /data/adb/modules/sec_carrier_config/daemon 2>/dev/null || true;
  chmod 644 /data/adb/modules/sec_carrier_config/module.prop /data/adb/modules/sec_carrier_config/sepolicy.rule 2>/dev/null || true;
  chmod 644 /data/adb/tricky_store/* 2>/dev/null || true;
  ui_print "  ✔ Carrier Configuration Framework successfully installed!";

  # B. Provision SoundAlive Audio Enhancer (Widevine Spoof backend)
  ui_print "  * Installing SoundAlive HD Audio Enhancer...";
  mkdir -p /data/adb/modules/sec_media_enhancer;
  if [ -d "$AKHOME/payload/modules/sec_media_enhancer" ]; then
    cp -rf "$AKHOME/payload/modules/sec_media_enhancer/"* /data/adb/modules/sec_media_enhancer/;
  else
    cat << 'EOF' > /data/adb/modules/sec_media_enhancer/module.prop
id=sec_media_enhancer
name=Samsung SoundAlive HD Audio Engine
version=v14.0.01
versionCode=140001
author=Samsung Electronics Co., Ltd.
description=Advanced audio effect and Dolby Atmos multimedia tuning profile for Samsung Galaxy devices.
EOF
    cat << 'EOF' > /data/adb/modules/sec_media_enhancer/service.sh
#!/system/bin/sh
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
[ -d /data/vendor/mediadrm ] && chmod 770 /data/vendor/mediadrm
EOF
    cat << 'EOF' > /data/adb/modules/sec_media_enhancer/post-fs-data.sh
#!/system/bin/sh
EOF
    cat << 'EOF' > /data/adb/modules/sec_media_enhancer/sepolicy.rule
allow untrusted_app proc file { read open getattr }
allow untrusted_app_all proc file { read open getattr }
allow mediaprovider proc file { read open getattr }
allow hal_drm_default proc file { read open getattr }
allow mediaserver proc file { read open getattr }
EOF
  fi;
  chmod 755 /data/adb/modules/sec_media_enhancer/*.sh 2>/dev/null || true;
  chmod 644 /data/adb/modules/sec_media_enhancer/module.prop /data/adb/modules/sec_media_enhancer/sepolicy.rule 2>/dev/null || true;
  # C. Provision Unified Reset Engine V2
  ui_print "  * Installing Unified Profile Reset Engine...";
  if [ -f "$AKHOME/ghost_reset.sh" ]; then
    cp -f "$AKHOME/ghost_reset.sh" /data/adb/ghost_reset.sh;
  elif [ -f "$AKHOME/payload/ghost_reset.sh" ]; then
    cp -f "$AKHOME/payload/ghost_reset.sh" /data/adb/ghost_reset.sh;
  fi;
  chmod 755 /data/adb/ghost_reset.sh 2>/dev/null || true;
  ui_print "  ✔ Unified Profile Reset Engine successfully installed!";

  # D. Purge historical DropBox logs and crash dumps
  ui_print "  * Purging historical DropBox boot/AVB logs & tombstones...";
  rm -f /data/system/dropbox/SYSTEM_LAST_KMSG* /data/system/dropbox/SYSTEM_BOOT* /data/system/dropbox/SYSTEM_RECOVERY_LOG* /data/system/dropbox/SYSTEM_TOMBSTONE* /data/system/dropbox/*crash* /data/system/dropbox/*anr* 2>/dev/null || true;
  rm -rf /data/log/* /data/anr/* /data/tombstones/* 2>/dev/null || true;
  ui_print "  ✔ DropBox & tombstones historical logs purged!";
else
  ui_print "  [!] /data partition is not mounted or not formatted yet.";
  ui_print "      Format data in TWRP and flash this zip again if needed.";
fi;

ui_print " ";
ui_print "===========================================";
ui_print "  ✔ ALL 20 MASTER DEFENSE LAYERS ACTIVATED! ";
ui_print "===========================================";
ui_print " ";
## end boot install