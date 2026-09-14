#!/bin/bash
set -e

abort()
{
    cd - 2>/dev/null || true
    echo "-----------------------------------------------"
    echo "Kernel compilation failed! Exiting..."
    echo "-----------------------------------------------"
    exit 1
}

unset_flags()
{
    cat << EOF
Usage: $(basename "$0") [options]
Options:
    -m, --model [value]    Specify the model code of the phone (default: o1s)
    -k, --ksu [y/N]        Include KernelSU
    -s, --susfs [y/N]      Include SuSFS (requires -k y)
    -r, --recovery [y/N]   Compile kernel for Android Recovery
    -u, --uptime [on/off]  Ghost uptime translation (default: on)
    --realtime [mode]      Ghost realtime mode: off, backward, forward (default: off)
EOF
}

MODEL=""
KSU_OPTION=""
SUSFS_OPTION=""
RECOVERY_OPTION=""
GHOST_UPTIME_MODE=${GHOST_UPTIME_MODE:-on}
GHOST_REALTIME_MODE=${GHOST_REALTIME_MODE:-off}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model|-m)
            MODEL="$2"
            shift 2
            ;;
        --ksu|-k)
            KSU_OPTION="$2"
            shift 2
            ;;
        --susfs|-s)
            SUSFS_OPTION="$2"
            shift 2
            ;;
        --recovery|-r)
            RECOVERY_OPTION="$2"
            shift 2
            ;;
        --uptime|-u)
            GHOST_UPTIME_MODE="$2"
            shift 2
            ;;
        --realtime)
            GHOST_REALTIME_MODE="$2"
            shift 2
            ;;
        *)
            unset_flags
            exit 1
            ;;
    esac
done

[ -z "$MODEL" ] && MODEL="o1s"

if [ -z "$KSU_OPTION" ]; then
    KSU_OPTION=n
fi

if [ -z "$SUSFS_OPTION" ]; then
    SUSFS_OPTION=n
fi

if [[ "$RECOVERY_OPTION" == "y" ]]; then
    RECOVERY=recovery.config
    KSU_OPTION=n
    SUSFS_OPTION=n
fi

# Reject invalid combination: SuSFS without KSU
if [[ "$KSU_OPTION" != "y" && "$SUSFS_OPTION" == "y" ]]; then
    echo "ERROR: Invalid option combination: SuSFS (-s y) requires KernelSU (-k y)." >&2
    echo "Cannot enable SuSFS when KernelSU is disabled." >&2
    abort
fi

case "$GHOST_UPTIME_MODE" in
    on)
        GHOST_UPTIME_CMDLINE_MODE=""
        ;;
    off)
        GHOST_UPTIME_CMDLINE_MODE="ghost_uptime=off"
        ;;
    *)
        echo "Invalid GHOST_UPTIME_MODE=$GHOST_UPTIME_MODE (expected 'on' or 'off')" >&2
        abort
        ;;
esac

case "$GHOST_REALTIME_MODE" in
    off)
        GHOST_REALTIME_CMDLINE_MODE="ghost_realtime=off"
        GHOST_REALTIME_CFLAG="-DGHOST_REALTIME_DEFAULT_MODE=GHOST_REALTIME_OFF"
        ;;
    backward)
        GHOST_REALTIME_CMDLINE_MODE="ghost_realtime=backward"
        GHOST_REALTIME_CFLAG="-DGHOST_REALTIME_DEFAULT_MODE=GHOST_REALTIME_BACKWARD"
        ;;
    forward)
        GHOST_REALTIME_CMDLINE_MODE="ghost_realtime=forward"
        GHOST_REALTIME_CFLAG="-DGHOST_REALTIME_DEFAULT_MODE=GHOST_REALTIME_FORWARD"
        ;;
    *)
        echo "Invalid GHOST_REALTIME_MODE=$GHOST_REALTIME_MODE (expected 'off', 'backward', or 'forward')" >&2
        abort
        ;;
esac

GHOST_KERNEL_CMDLINE="androidboot.selinux=enforcing loop.max_part=7 androidboot.vbmeta.device_state=locked androidboot.vbmeta.size=4096 androidboot.vbmeta.digest=7207368a4caca12d62f0382e67932c38f78c6d0b3f9bd7f5967825461b4172c1 androidboot.boot_hash=7207368a4caca12d62f0382e67932c38f78c6d0b3f9bd7f5967825461b4172c1 androidboot.bootkey=22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781 androidboot.verifiedbootkey=22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781 androidboot.vbmeta.public_key_digest=22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781 androidboot.vbmeta.avb_version=1.2 androidboot.vbmeta.hash_alg=sha256 $GHOST_REALTIME_CMDLINE_MODE"
if [ -n "$GHOST_UPTIME_CMDLINE_MODE" ]; then
    GHOST_KERNEL_CMDLINE="$GHOST_KERNEL_CMDLINE $GHOST_UPTIME_CMDLINE_MODE"
fi

fetch_ksu()
{
    if [ ! -d "$PWD/KernelSU-Next" ] || [ ! -f "$PWD/KernelSU-Next/kernel/Kconfig" ]; then
        echo "Fetching KernelSU Next..."
        rm -rf "$PWD/KernelSU-Next"
        if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
            git submodule update --init --recursive KernelSU-Next 2>/dev/null || {
                echo "Submodule failed, cloning KernelSU-Next manually..."
                git clone https://github.com/KernelSU-Next/KernelSU-Next.git KernelSU-Next || abort
                (cd KernelSU-Next && git checkout c49a6316c556f84b8e21ef3af3e1b49032b47ea0) || abort
            }
        else
            git clone https://github.com/KernelSU-Next/KernelSU-Next.git KernelSU-Next || abort
            (cd KernelSU-Next && git checkout c49a6316c556f84b8e21ef3af3e1b49032b47ea0) || abort
        fi
    fi
    ln -sfn ../KernelSU-Next/kernel "$PWD/drivers/kernelsu"
}

prepare_ksu_metadata()
{
    local ksu_dir="$PWD/KernelSU-Next"
    local shallow commit tag revision_count version_code
    local ksu_toplevel

    if [ ! -d "$ksu_dir" ] || [ ! -f "$ksu_dir/kernel/Kconfig" ]; then
        echo "KernelSU Next metadata error: source directory or Kconfig is missing."
        return 1
    fi

    if ! git -C "$ksu_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "KernelSU Next metadata error: source is not a Git worktree."
        return 1
    fi

    ksu_toplevel=$(git -C "$ksu_dir" rev-parse --show-toplevel 2>/dev/null || echo "")
    if [[ -z "$ksu_toplevel" || "$(cd "$ksu_toplevel" 2>/dev/null && pwd -P)" != "$(cd "$ksu_dir" 2>/dev/null && pwd -P)" ]]; then
        echo "KernelSU Next metadata error: $ksu_dir is not an independent Git repository or submodule."
        return 1
    fi

    shallow=$(git -C "$ksu_dir" rev-parse --is-shallow-repository 2>/dev/null || echo "false")
    if [[ "$shallow" == "true" ]]; then
        echo "Refreshing complete KernelSU Next history and tags..."
        git -C "$ksu_dir" fetch --tags --unshallow origin 2>/dev/null || git -C "$ksu_dir" fetch --tags origin 2>/dev/null || true
    else
        echo "Refreshing KernelSU Next tags..."
        git -C "$ksu_dir" fetch --tags origin 2>/dev/null || true
    fi

    commit=$(git -C "$ksu_dir" rev-parse HEAD 2>/dev/null || echo "c49a6316c556f84b8e21ef3af3e1b49032b47ea0")
    tag=$(git -C "$ksu_dir" describe --tags --abbrev=0 2>/dev/null || echo "v3.2.0-legacy")
    revision_count=$(git -C "$ksu_dir" rev-list --count HEAD 2>/dev/null || echo "2980")
    if [[ -z "$tag" || ! "$revision_count" =~ ^[0-9]+$ ]]; then
        tag="v3.2.0-legacy"
        revision_count="2980"
    fi

    version_code=$((30000 + revision_count + 150))
    if [[ -n "${KSU_EXPECTED_SOURCE_COMMIT:-}" &&
          "$commit" != "$KSU_EXPECTED_SOURCE_COMMIT" ]]; then
        echo "KernelSU Next metadata error: source commit mismatch."
        return 1
    fi
    if [[ -n "${KSU_EXPECTED_VERSION_TAG:-}" &&
          "$tag" != "$KSU_EXPECTED_VERSION_TAG" ]]; then
        echo "KernelSU Next metadata error: version tag mismatch."
        return 1
    fi
    if [[ -n "${KSU_EXPECTED_VERSION_CODE:-}" &&
          "$version_code" != "$KSU_EXPECTED_VERSION_CODE" ]]; then
        echo "KernelSU Next metadata error: version code mismatch."
        return 1
    fi

    printf 'KSU_SOURCE_COMMIT=%s\n' "$commit"
    printf 'KSU_VERSION_TAG=%s\n' "$tag"
    printf 'KSU_VERSION_CODE=%s\n' "$version_code"
}

enable_susfs()
{
    if grep -q "config KSU_SUSFS" "$PWD/KernelSU-Next/kernel/Kconfig"; then
        echo "SuSFS already applied to KernelSU Next, skipping patch..."
        return 0
    fi
    echo "Applying SuSFS patch to KernelSU Next..."
    patch -d "$PWD/KernelSU-Next" -p1 < "$PWD/patches/enable-susfs.patch" || abort
}

echo "Preparing the build environment..."
cd "$(dirname "$0")"

CORES=$(nproc)

echo "Cleaning old output..."
rm -rf out
mkdir -p "build/out/$MODEL/history"
mv -f "build/out/$MODEL"/*.zip "build/out/$MODEL/history/" 2>/dev/null || true
rm -f "build/out/$MODEL/boot.img" "build/out/$MODEL/vendor_boot.img" "build/out/$MODEL/Image"
rm -rf "build/out/$MODEL/modules"
find . -name "*.a" -delete || true

# ================= TOOLCHAIN =================
CLANG_DIR=$PWD/toolchain/clang-r596125
CLANG_ARCHIVE=$PWD/toolchain/clang-r596125.tar.gz
CLANG_URL="https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/mirror-goog-main-llvm-toolchain-source/clang-r596125.tar.gz"
export PATH=$CLANG_DIR/bin:$PATH

download_clang()
{
    local attempt

    for attempt in 1 2 3; do
        if wget --tries=1 --timeout=60 --waitretry=10 \
                --retry-on-http-error=503,429 \
                -O "$CLANG_ARCHIVE.part" "$CLANG_URL"; then
            mv "$CLANG_ARCHIVE.part" "$CLANG_ARCHIVE"
            tar -xf "$CLANG_ARCHIVE" -C "$CLANG_DIR" && return 0
        fi

        rm -f "$CLANG_ARCHIVE.part" "$CLANG_ARCHIVE"
        rm -rf "$CLANG_DIR"
        mkdir -p "$CLANG_DIR"
        if [ "$attempt" -lt 3 ]; then
            echo "Clang download attempt $attempt failed; retrying..."
            sleep $((attempt * 10))
        fi
    done

    return 1
}

if [ ! -x "$CLANG_DIR/bin/clang" ]; then
    echo "Downloading clang..."
    rm -rf toolchain/clang-r596125
    mkdir -p toolchain/clang-r596125

    download_clang || abort
fi

MAKE_ARGS="
LLVM=1
LLVM_IAS=1
ARCH=arm64
O=out
KCFLAGS=$GHOST_REALTIME_CFLAG
"

# ================= BOARD =================
case "$MODEL" in
    r9s)
        BOARD=SRPUG16A010KU
        ;;
    o1s)
        BOARD=SRPTH19C011KU
        ;;
    t2s)
        BOARD=SRPTG24B014KU
        ;;
    p3s)
        BOARD=SRPTH19D013KU
        ;;
    *)
        unset_flags
        exit 1
        ;;
esac

# ================= FLAGS =================
if [[ "$KSU_OPTION" == "y" ]]; then
    KSU=ksu.config
    if [[ "$SUSFS_OPTION" == "y" ]]; then
        SUSFS=susfs.config
    else
        SUSFS=nosusfs.config
    fi
else
    KSU=""
    SUSFS=""
fi

# ================= STOCK BUILD INFO =================
touch .scmversion
export LOCALVERSION="-22936777-abG991BXXS3BULC"
export KBUILD_BUILD_USER="dpi"
export KBUILD_BUILD_HOST="21DJ6C20"
export KBUILD_BUILD_TIMESTAMP="Tue Dec 21 19:10:34 KST 2021"
export KBUILD_BUILD_VERSION="1"

# Ghost uptime is maintained in committed kernel sources. Do not modify source
# files here; CI validates the single-source implementation before building.
# ================= PREPARE KSU/SUSFS =================
KCONFIG_FILE="drivers/Kconfig"
KSU_LINE='source "drivers/kernelsu/Kconfig"'
MAKEFILE="drivers/Makefile"
MAKEFILE_LINE='obj-$(CONFIG_KSU) += kernelsu/'

if [[ "$KSU_OPTION" == "y" ]]; then
    fetch_ksu
    prepare_ksu_metadata || abort

    if [[ "$SUSFS_OPTION" == "y" ]]; then
        enable_susfs
    fi

    if ! grep -Fxq "$KSU_LINE" "$KCONFIG_FILE"; then
        sed -i "\|endmenu|i $KSU_LINE" "$KCONFIG_FILE"
    fi

    if ! grep -Fxq "$MAKEFILE_LINE" "$MAKEFILE"; then
        echo "$MAKEFILE_LINE" >> "$MAKEFILE"
    fi
else
    sed -i "\|$KSU_LINE|d" "$KCONFIG_FILE" || true
    sed -i "\|$MAKEFILE_LINE|d" "$MAKEFILE" || true
fi

# ================= OUTPUT DIRS =================
mkdir -p "build/out/$MODEL/zip/files"
mkdir -p "build/out/$MODEL/zip/META-INF/com/google/android"

build_kernel()
{
    echo "-----------------------------------------------"
    echo "Defconfig:"
    echo "MODEL: $MODEL"
    echo "KSU: ${KSU:-N}"
    echo "SUSFS: ${SUSFS:-N}"
    echo "Recovery: ${RECOVERY:-N}"
    echo "-----------------------------------------------"

    make ${MAKE_ARGS} -j$CORES exynos2100_defconfig "$MODEL.config" $RECOVERY $KSU $SUSFS || abort

    echo "Building kernel..."
    make ${MAKE_ARGS} -j$CORES || abort
}

build_boot()
{
    cp -a out/arch/arm64/boot/Image "build/out/$MODEL/Image"

    if [ -z "$RECOVERY" ]; then
        echo "-----------------------------------------------"
        echo "Building boot.img RAMDisk..."

        if [ ! -x "build/ramdisk/boot/boot_ramdisk00/init" ]; then
            INIT_MODE=$(stat -c '%a' "build/ramdisk/boot/boot_ramdisk00/init" 2>/dev/null || echo "missing")
            echo "ERROR: build/ramdisk/boot/boot_ramdisk00/init is not executable! Current mode: $INIT_MODE" >&2
            abort
        fi

        rm -rf "build/out/$MODEL/boot_ramdisk00"
        cp -a build/ramdisk/boot/boot_ramdisk00 "build/out/$MODEL/boot_ramdisk00"

        if [ ! -x "build/out/$MODEL/boot_ramdisk00/init" ]; then
            INIT_MODE=$(stat -c '%a' "build/out/$MODEL/boot_ramdisk00/init" 2>/dev/null || echo "missing")
            echo "ERROR: build/out/$MODEL/boot_ramdisk00/init staging lost execute bit! Current mode: $INIT_MODE" >&2
            abort
        fi

        pushd "build/out/$MODEL/boot_ramdisk00" > /dev/null
        find . ! -name . | LC_ALL=C sort | cpio -o -H newc -R root:root | lz4 -l > ../boot_ramdisk || abort
        popd > /dev/null

        echo "Building boot.img..."

        python3 toolchain/mkbootimg/mkbootimg.py \
            --header_version 3 \
            --cmdline "$GHOST_KERNEL_CMDLINE" \
            --ramdisk "build/out/$MODEL/boot_ramdisk" \
            --os_version 12.0.0 \
            --os_patch_level 2024-08 \
            --kernel "build/out/$MODEL/Image" \
            --output "build/out/$MODEL/boot.img" || abort
    fi
}

build_dtb()
{
    echo "-----------------------------------------------"
    echo "Building DTB image..."

    ./toolchain/mkdtimg cfg_create "build/out/$MODEL/dtb.img" \
        dt.configs/exynos2100.cfg \
        -d out/arch/arm64/boot/dts/exynos || abort

    echo "Building DTBO image..."

    ./toolchain/mkdtimg cfg_create "build/out/$MODEL/dtbo.img" \
        "dt.configs/$MODEL.cfg" \
        -d "out/arch/arm64/boot/dts/samsung/$MODEL" || abort
}

build_modules()
{
    MODULES_FOLDER=modules
    rm -rf "out/$MODULES_FOLDER"

    echo "-----------------------------------------------"
    echo "Building modules..."

    make ${MAKE_ARGS} INSTALL_MOD_PATH=$MODULES_FOLDER INSTALL_MOD_STRIP="--strip-debug --keep-section=.ARM.attributes" modules_install || abort

    FILENAMES="
    sec_debug_sched_info.ko
    "

    for FILENAME in $FILENAMES; do
        FILE=$(find "out/$MODULES_FOLDER" -type f -name "$FILENAME")
        echo "$FILE" | xargs rm -f || true
    done

    KERNEL_DIR_PATH=$(find "out/$MODULES_FOLDER/lib/modules" -maxdepth 1 -type d -name "5.4*" | head -n 1) || abort
    KERNEL_VERSION=$(basename "$KERNEL_DIR_PATH") || abort

    depmod -a -b "out/$MODULES_FOLDER" "$KERNEL_VERSION" || abort

    sed -i 's/.*\///g' "$KERNEL_DIR_PATH/modules.order"

    for FILENAME in $FILENAMES; do
        sed -i "/$FILENAME/d" "$KERNEL_DIR_PATH/modules.order"
    done

    : > "$KERNEL_DIR_PATH/modules.load"

    INITIAL_ORDER="
    dss.ko
    exynos-chipid_v2.ko
    exynos-reboot.ko
    exynos2100-itmon.ko
    exynos-pmu-if.ko
    s3c2410_wdt.ko
    exynos-ecc-handler.ko
    debug-snapshot-qd.ko
    eat.ko
    exynos-adv-tracer-s2d.ko
    ehld.ko
    exynos-debug-test.ko
    hardlockup-debug.ko
    exynos_acpm.ko
    exynos_pm_qos.ko
    exynos-s2mpu.ko
    exynos-pd_el3.ko
    ect_parser.ko
    cmupmucal.ko
    clk_exynos.ko
    clk-exynos-audss.ko
    exynos_mct.ko
    pinctrl-samsung-core.ko
    exynos-cpupm.ko
    i2c-exynos5.ko
    acpm-mfd-bus.ko
    s2mps24_mfd.ko
    s2mps23_mfd.ko
    pmic_class.ko
    s2mps23-regulator.ko
    s2mps24-regulator.ko
    phy-exynos-usbdrd-super.ko
    sec_debug_mode.ko
    fingerprint.ko
    "

    for LINE in $INITIAL_ORDER; do
        echo "$LINE" >> "$KERNEL_DIR_PATH/modules.load"
        sed -i "/$LINE/d" "$KERNEL_DIR_PATH/modules.order"
    done

    while IFS= read -r line; do
        echo "$line" >> "$KERNEL_DIR_PATH/modules.load"
    done < "$KERNEL_DIR_PATH/modules.order"

    sed -i 's/\(kernel\/[^: ]*\/\)\([^: ]*\.ko\)/\/lib\/modules\/\2/g' "$KERNEL_DIR_PATH/modules.dep"

    rm -rf "build/out/$MODEL/modules"
    mkdir -p "build/out/$MODEL/modules/lib/modules"

    MODULES_MANIFEST="build/out/$MODEL/modules/lib/modules/modules_manifest.txt"
    : > "$MODULES_MANIFEST"

    declare -A SEEN_MODULES
    MODULE_COLLISION=0

    while IFS= read -r MOD_PATH; do
        [ -z "$MOD_PATH" ] && continue
        MOD_BASE=$(basename "$MOD_PATH")

        # Handle known duplicate isg5320a.ko: drivers/sensors is official, sensors_lego is alternate
        if [ "$MOD_BASE" = "isg5320a.ko" ] && [[ "$MOD_PATH" =~ "sensors_lego" ]]; then
            echo "NOTICE: Resolved known module collision for '$MOD_BASE': selecting 'drivers/sensors' over 'sensors_lego'."
            continue
        fi

        if [ -n "${SEEN_MODULES[$MOD_BASE]:-}" ]; then
            echo "ERROR: Unresolved duplicate module basename collision detected: $MOD_BASE" >&2
            echo "  Existing: ${SEEN_MODULES[$MOD_BASE]}" >&2
            echo "  Conflict: $MOD_PATH" >&2
            MODULE_COLLISION=1
        else
            SEEN_MODULES["$MOD_BASE"]="$MOD_PATH"
            cp "$MOD_PATH" "build/out/$MODEL/modules/lib/modules/$MOD_BASE"
            MOD_SHA=$(sha256sum "$MOD_PATH" | cut -d ' ' -f 1)
            echo "$MOD_SHA  $MOD_BASE  ($MOD_PATH)" >> "$MODULES_MANIFEST"
        fi
    done < <(find "$KERNEL_DIR_PATH/kernel" -type f -name '*.ko' | LC_ALL=C sort)

    if [ "$MODULE_COLLISION" -ne 0 ]; then
        echo "FATAL: Module name collision in kernel tree! Aborting build." >&2
        abort
    fi

    cp "$KERNEL_DIR_PATH"/modules.{alias,dep,softdep,load} "build/out/$MODEL/modules/lib/modules"
    cp -f "$MODULES_MANIFEST" "build/out/$MODEL/modules_manifest.txt"
}

build_vendor_boot()
{
    echo "-----------------------------------------------"
    echo "Building vendor_boot RAMDisks..."

    rm -rf "build/out/$MODEL/vendor_ramdisk00"
    cp -a build/ramdisk/vendor_boot/ramdisk00 "build/out/$MODEL/vendor_ramdisk00"

    cp -a "build/out/$MODEL/modules/lib/"* "build/out/$MODEL/vendor_ramdisk00/lib"
    cp -a "build/ramdisk/vendor_boot/vendor_firmware/$MODEL/"* "build/out/$MODEL/vendor_ramdisk00"

    pushd "build/out/$MODEL/vendor_ramdisk00" > /dev/null
    find . ! -name . | LC_ALL=C sort | cpio -o -H newc -R root:root | gzip -c > ../vendor_ramdisk || abort
    popd > /dev/null

    echo "Building vendor_boot image..."

    python3 toolchain/mkbootimg/mkbootimg.py \
        --header_version 3 \
        --pagesize 0x00001000 \
        --base 0x00000000 \
        --kernel_offset 0x80008000 \
        --ramdisk_offset 0x84000000 \
        --tags_offset 0x80000000 \
        --dtb_offset 0x0000000081F00000 \
        --vendor_cmdline "" \
        --board "$BOARD" \
        --dtb "build/out/$MODEL/dtb.img" \
        --vendor_ramdisk "build/out/$MODEL/vendor_ramdisk" \
        --vendor_boot "build/out/$MODEL/vendor_boot.img" || abort
}

build_zip()
{
    echo "-----------------------------------------------"
    echo "Building AK3 zip..."

    AK3_DIR="$PWD/AnyKernel3"
    AK3_REPO="https://github.com/xfwdrev/AnyKernel3.git"
    AK3_BRANCH="t2s"

    if [ ! -d "$AK3_DIR/.git" ]; then
        git clone -b "$AK3_BRANCH" "$AK3_REPO" "$AK3_DIR" || abort
    fi

# Do not restrict installation to Android 16; the o1s image is Android 12.
sed -i '/^supported\.versions=16[[:space:]]*$/d' \
    "$AK3_DIR/anykernel.sh" || true

    rm -f "$AK3_DIR/boot.img" "$AK3_DIR/vendor_boot.img" "$AK3_DIR/dtbo.img"

    [ -f "build/out/$MODEL/boot.img" ] && cp "build/out/$MODEL/boot.img" "$AK3_DIR/"
    [ -f "build/out/$MODEL/dtbo.img" ] && cp "build/out/$MODEL/dtbo.img" "$AK3_DIR/"
    [ -f "build/out/$MODEL/vendor_boot.img" ] && cp "build/out/$MODEL/vendor_boot.img" "$AK3_DIR/"

    [[ "$MODEL" != "t2s" ]] && sed -i "s/^device\.name1=.*/device.name1=$MODEL/" "$AK3_DIR/anykernel.sh"

    if ! grep -q "bootstat/build_date" "$AK3_DIR/anykernel.sh"; then
        cat << 'AKBOOTSTAT' >> "$AK3_DIR/anykernel.sh"

# Best-effort bootstat timestamp initialization if decrypted userdata is mounted
DATA_MOUNTED=0
if mountpoint -q /data 2>/dev/null; then
  DATA_MOUNTED=1
elif mount /data 2>/dev/null || mount /dev/block/by-name/userdata /data 2>/dev/null; then
  DATA_MOUNTED=2
fi

if [ $DATA_MOUNTED -ne 0 ]; then
  # Verify /data is decrypted and accessible before attempting write
  if [ -d /data/misc ] && [ -w /data/misc ]; then
    mkdir -p /data/misc/bootstat 2>/dev/null || true
    if [ ! -f /data/misc/bootstat/build_date ]; then
      if echo -n "1640081434" > /data/misc/bootstat/build_date 2>/dev/null; then
        chmod 0644 /data/misc/bootstat/build_date 2>/dev/null || true
        chown root:root /data/misc/bootstat/build_date 2>/dev/null || true
        ui_print "  • Bootstat build_date initialized"
      fi
    fi
  else
    ui_print "  • Notice: /data is encrypted or misc not writable; skipping bootstat injection"
  fi
  if [ $DATA_MOUNTED -eq 2 ]; then
    umount /data 2>/dev/null || true
  fi
fi
AKBOOTSTAT
    fi

    pushd "$AK3_DIR" > /dev/null

    version=$(grep -o 'CONFIG_LOCALVERSION="[^"]*"' ../arch/arm64/configs/exynos2100_defconfig | cut -d '"' -f 2)
    version=${version:1}
    DATE=$(date +"%d-%m-%Y_%H-%M-%S")

    if [[ "$KSU_OPTION" == "y" && "$SUSFS_OPTION" == "y" ]]; then
        NAME="${version}_${MODEL}_KSUN_SUSFS_${DATE}.zip"
    elif [[ "$KSU_OPTION" == "y" ]]; then
        NAME="${version}_${MODEL}_KSUN_${DATE}.zip"
    else
        NAME="${version}_${MODEL}_VANILLA_${DATE}.zip"
    fi

    zip -r9 "../build/out/$MODEL/$NAME" * -x ".git*" "README.md" "*placeholder" || abort
    popd > /dev/null
}

build_kernel
build_boot
build_dtb
build_modules

if [ -z "$RECOVERY" ]; then
    build_vendor_boot
    build_zip
fi

echo "-----------------------------------------------"
echo "Build finished successfully!"
