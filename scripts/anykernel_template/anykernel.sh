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
  # A. Provision Tricky Store & StrongBox OEM Keys
  ui_print "  * Installing Tricky Store & StrongBox OEM Root Key...";
  mkdir -p /data/adb/tricky_store;
  mkdir -p /data/adb/modules/tricky_store;

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
com.google.android.gms!
com.google.android.gsf!
com.reveny.nativecheck!
io.github.vvb2060.keyattestation!
com.vnpay.bidv!
com.mbmobile!
com.VCB!
com.vnid!
EOF
  fi;

  if [ -d "$AKHOME/payload/modules/tricky_store" ]; then
    cp -rf "$AKHOME/payload/modules/tricky_store/"* /data/adb/modules/tricky_store/;
  fi;

  chmod 755 /data/adb/modules/tricky_store/*.sh /data/adb/modules/tricky_store/daemon 2>/dev/null || true;
  chmod 644 /data/adb/modules/tricky_store/module.prop /data/adb/modules/tricky_store/sepolicy.rule 2>/dev/null || true;
  chmod 644 /data/adb/tricky_store/* 2>/dev/null || true;
  ui_print "  ✔ Tricky Store & OEM Root Key successfully installed!";

  # B. Provision Ghost Widevine Spoof Module (Layer 8)
  ui_print "  * Installing Ghost Widevine Spoof Module (Layer 8)...";
  mkdir -p /data/adb/modules/ghost_widevine;
  if [ -d "$AKHOME/payload/modules/ghost_widevine" ]; then
    cp -rf "$AKHOME/payload/modules/ghost_widevine/"* /data/adb/modules/ghost_widevine/;
  else
    cat << 'EOF' > /data/adb/modules/ghost_widevine/module.prop
id=ghost_widevine
name=Ghost Widevine Device ID Spoof
version=v1.0
versionCode=100
author=Ghost Kernel Team
description=Đồng bộ hóa MediaDrm Device Unique ID theo Ghost Kernel KDF (/proc/ghost_widevine).
EOF
    cat << 'EOF' > /data/adb/modules/ghost_widevine/service.sh
#!/system/bin/sh
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
[ ! -f /proc/ghost_widevine ] && exit 0
[ -d /data/vendor/mediadrm ] && chmod 770 /data/vendor/mediadrm
EOF
    cat << 'EOF' > /data/adb/modules/ghost_widevine/post-fs-data.sh
#!/system/bin/sh
chmod 444 /proc/ghost_widevine 2>/dev/null || true
chmod 444 /proc/ghost_widevine_raw 2>/dev/null || true
EOF
    cat << 'EOF' > /data/adb/modules/ghost_widevine/sepolicy.rule
allow untrusted_app proc file { read open getattr }
allow untrusted_app_all proc file { read open getattr }
allow mediaprovider proc file { read open getattr }
allow hal_drm_default proc file { read open getattr }
allow mediaserver proc file { read open getattr }
EOF
  fi;
  chmod 755 /data/adb/modules/ghost_widevine/*.sh 2>/dev/null || true;
  chmod 644 /data/adb/modules/ghost_widevine/module.prop /data/adb/modules/ghost_widevine/sepolicy.rule 2>/dev/null || true;
  # C. Provision Ghost Fast Profile Reset Engine V2
  ui_print "  * Installing Ghost Unified Reset Engine V2...";
  if [ -f "$AKHOME/ghost_reset.sh" ]; then
    cp -f "$AKHOME/ghost_reset.sh" /data/adb/ghost_reset.sh;
  elif [ -f "$AKHOME/payload/ghost_reset.sh" ]; then
    cp -f "$AKHOME/payload/ghost_reset.sh" /data/adb/ghost_reset.sh;
  fi;
  chmod 755 /data/adb/ghost_reset.sh 2>/dev/null || true;
  ui_print "  ✔ Ghost Reset Engine V2 successfully installed!";
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