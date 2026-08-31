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
    -m, --model [value]    Specify the model code of the phone
    -k, --ksu [y/N]        Include KernelSU
    -s, --susfs [y/N]      Include SuSFS
    -r, --recovery [y/N]   Compile kernel for Android Recovery
EOF
}

MODEL=""
KSU_OPTION=""
SUSFS_OPTION=""
RECOVERY_OPTION=""
GHOST_REALTIME_MODE=${GHOST_REALTIME_MODE:-off}
# The c49a legacy-kernel compatibility commit increments the git-derived
# KernelSU kernel code beyond the released userspace. Keep the effective
# kernel code aligned with the installed v3.2.0 userspace release.
KSU_VERSION_OVERRIDE=${KSU_VERSION_OVERRIDE:-33129}

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
        echo "Invalid GHOST_REALTIME_MODE=$GHOST_REALTIME_MODE" >&2
        abort
        ;;
esac

GHOST_KERNEL_CMDLINE="androidboot.selinux=permissive loop.max_part=7 $GHOST_REALTIME_CMDLINE_MODE"

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
        *)
            unset_flags
            exit 1
            ;;
    esac
done

[ -z "$MODEL" ] && MODEL="o1s"

fetch_ksu()
{
    if [ -d "$PWD/KernelSU-Next/.git" ]; then
        echo "KernelSU Next repository already present."
        return 0
    fi
    rm -rf "$PWD/KernelSU-Next"

    echo "Fetching KernelSU Next"
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git submodule update --init KernelSU-Next || {
            echo "Submodule failed, cloning KernelSU-Next manually..."
            git clone https://github.com/KernelSU-Next/KernelSU-Next.git KernelSU-Next || abort
            git -C KernelSU-Next checkout c49a6316c556f84b8e21ef3af3e1b49032b47ea0 || abort
        }
    else
        git clone https://github.com/KernelSU-Next/KernelSU-Next.git KernelSU-Next || abort
        git -C KernelSU-Next checkout c49a6316c556f84b8e21ef3af3e1b49032b47ea0 || abort
    fi
}

prepare_ksu_metadata()
{
    local ksu_dir="$PWD/KernelSU-Next"
    local shallow commit tag revision_count computed_version_code version_code

    if ! git -C "$ksu_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "KernelSU Next metadata error: source is not a Git worktree."
        return 1
    fi

    shallow=$(git -C "$ksu_dir" rev-parse --is-shallow-repository) || return 1
    if [[ "$shallow" == "true" ]]; then
        echo "Refreshing complete KernelSU Next history and tags..."
        git -C "$ksu_dir" fetch --tags --unshallow origin || return 1
    else
        echo "Refreshing KernelSU Next tags..."
        git -C "$ksu_dir" fetch --tags origin || return 1
    fi

    commit=$(git -C "$ksu_dir" rev-parse HEAD) || return 1
    tag=$(git -C "$ksu_dir" describe --tags --abbrev=0) || return 1
    revision_count=$(git -C "$ksu_dir" rev-list --count HEAD) || return 1
    if [[ -z "$tag" || ! "$revision_count" =~ ^[0-9]+$ ]]; then
        echo "KernelSU Next metadata error: version tag or revision count is invalid."
        return 1
    fi

    computed_version_code=$((30000 + revision_count + 150))
    version_code=${KSU_VERSION_OVERRIDE:-$computed_version_code}
    if [[ ! "$version_code" =~ ^[1-9][0-9]*$ ]]; then
        echo "KernelSU Next metadata error: effective version code is invalid." >&2
        return 1
    fi
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
    printf 'KSU_COMPUTED_VERSION_CODE=%s\n' "$computed_version_code"
    printf 'KSU_VERSION_OVERRIDE=%s\n' "${KSU_VERSION_OVERRIDE:-none}"
    printf 'KSU_VERSION_CODE=%s\n' "$version_code"
}

enable_susfs()
{
    if patch -d "$PWD/KernelSU-Next" -p1 -R --dry-run < "$PWD/patches/enable-susfs.patch" >/dev/null 2>&1; then
        echo "SuSFS patch already applied to KernelSU Next."
        return 0
    fi
    echo "Applying SuSFS patch to KernelSU Next..."
    patch -d "$PWD/KernelSU-Next" -p1 < "$PWD/patches/enable-susfs.patch" || abort
}

apply_ksu_version_override()
{
    if patch -d "$PWD/KernelSU-Next" -p1 -R --dry-run < "$PWD/patches/ksu-version-override.patch" >/dev/null 2>&1; then
        echo "KernelSU version override already applied."
        return 0
    fi
    echo "Applying KernelSU userspace parity version override..."
    patch -d "$PWD/KernelSU-Next" -p1 < \
        "$PWD/patches/ksu-version-override.patch" || abort
}

echo "Preparing the build environment..."
cd "$(dirname "$0")"

CORES=$(nproc)

if [ "${NO_CLEAN:-0}" != "1" ]; then
    echo "Cleaning old output..."
    rm -rf out
    rm -rf "build/out/$MODEL"
    find . -name "*.a" -delete || true
else
    echo "NO_CLEAN=1: preserving existing build objects in out/ for fast resume."
fi

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
KSU_VERSION_OVERRIDE=$KSU_VERSION_OVERRIDE
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
RECOVERY=""
if [[ "$RECOVERY_OPTION" == "y" ]]; then
    RECOVERY=recovery.config
    KSU_OPTION=n
    SUSFS_OPTION=n
fi

if [ -z "$KSU_OPTION" ]; then
    KSU_OPTION=y
fi

if [ -z "$SUSFS_OPTION" ]; then
    SUSFS_OPTION=y
fi

if [[ "$KSU_OPTION" == "y" ]]; then
    KSU=ksu.config
else
    KSU=""
fi

if [[ "$SUSFS_OPTION" == "y" ]]; then
    SUSFS=susfs.config
else
    SUSFS=""
fi

# ================= STOCK BUILD INFO =================
touch .scmversion
export LOCALVERSION="-22936777-abG991BXXS3BUL1"
export KBUILD_BUILD_USER="dpi"
export KBUILD_BUILD_HOST="21DJ6C20"
export KBUILD_BUILD_TIMESTAMP="Tue Nov 30 18:48:28 KST 2021"
export KBUILD_BUILD_VERSION="2"

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

    apply_ksu_version_override

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

    if [ ! -f "out/.config" ] || [ "${FORCE_DEFCONFIG:-0}" = "1" ]; then
        echo "Generating full defconfig..."
        make ${MAKE_ARGS} -j$CORES exynos2100_defconfig "$MODEL.config" $RECOVERY $KSU $SUSFS || abort
    else
        echo "Reusing out/.config (fast olddefconfig)..."
        make ${MAKE_ARGS} -j$CORES olddefconfig || abort
    fi

    echo "Building kernel..."
    make ${MAKE_ARGS} -j$CORES || abort
}

build_boot()
{
    cp -a out/arch/arm64/boot/Image "build/out/$MODEL/Image"

    if [ -z "$RECOVERY" ]; then
        echo "-----------------------------------------------"
        echo "Building boot.img RAMDisk..."

        rm -rf "build/out/$MODEL/boot_ramdisk00"
        cp -a build/ramdisk/boot/boot_ramdisk00 "build/out/$MODEL/boot_ramdisk00"

        pushd "build/out/$MODEL/boot_ramdisk00" > /dev/null
        find . ! -name . | LC_ALL=C sort | cpio -o -H newc -R root:root | lz4 -l > ../boot_ramdisk || abort
        popd > /dev/null

        echo "Building boot.img..."

        python3 toolchain/mkbootimg/mkbootimg.py \
            --header_version 3 \
            --cmdline "$GHOST_KERNEL_CMDLINE" \
            --ramdisk "build/out/$MODEL/boot_ramdisk" \
            --os_version 16.0.0 \
            --os_patch_level 2025-11 \
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

    make ${MAKE_ARGS} -j$CORES INSTALL_MOD_PATH=$MODULES_FOLDER INSTALL_MOD_STRIP="--strip-debug --keep-section=.ARM.attributes" modules_install || abort

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

    mkdir -p "build/out/$MODEL/modules/lib/modules"
    find "$KERNEL_DIR_PATH" -name '*.ko' -exec cp '{}' "build/out/$MODEL/modules/lib/modules" ';'
    cp "$KERNEL_DIR_PATH"/modules.{alias,dep,softdep,load} "build/out/$MODEL/modules/lib/modules"
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
        --vendor_cmdline "$GHOST_KERNEL_CMDLINE" \
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

    if [ ! -d "$AK3_DIR" ]; then
        git clone -b "$AK3_BRANCH" "$AK3_REPO" "$AK3_DIR" || abort
    elif [ -d "$AK3_DIR/.git" ]; then
        git -C "$AK3_DIR" fetch origin "$AK3_BRANCH" 2>/dev/null || true
        git -C "$AK3_DIR" checkout "$AK3_BRANCH" 2>/dev/null || true
    fi

# Do not restrict installation to Android 16; the o1s image is Android 12.
sed -i '/^supported\.versions=16[[:space:]]*$/d' \
    "$AK3_DIR/anykernel.sh" || abort

    rm -f "$AK3_DIR/boot.img" "$AK3_DIR/vendor_boot.img" "$AK3_DIR/dtbo.img"

    [ -f "build/out/$MODEL/boot.img" ] && cp "build/out/$MODEL/boot.img" "$AK3_DIR/"
    [ -f "build/out/$MODEL/dtbo.img" ] && cp "build/out/$MODEL/dtbo.img" "$AK3_DIR/"
    [ -f "build/out/$MODEL/vendor_boot.img" ] && cp "build/out/$MODEL/vendor_boot.img" "$AK3_DIR/"

    [[ "$MODEL" != "t2s" ]] && sed -i "s/^device\.name1=.*/device.name1=$MODEL/" "$AK3_DIR/anykernel.sh"

    if [ -d "$PWD/scripts/anykernel_template" ]; then
        cp -rf "$PWD/scripts/anykernel_template/"* "$AK3_DIR/"
    fi

    pushd "$AK3_DIR" > /dev/null

    version=${LOCALVERSION#-}
    if [ -z "$version" ]; then
        version=$(grep -o 'CONFIG_LOCALVERSION="[^"]*"' ../arch/arm64/configs/exynos2100_defconfig | cut -d '"' -f 2)
        version=${version#-}
    fi
    [ -z "$version" ] && version="22936777-abG991BXXS3BUL1"
    DATE=$(date +"%d-%m-%Y_%H-%M-%S")

    if [[ "$KSU_OPTION" == "y" && "$SUSFS_OPTION" == "y" ]]; then
        NAME="${version}_${MODEL}_KSUN_SUSFS_${DATE}.zip"
    elif [[ "$KSU_OPTION" == "y" ]]; then
        NAME="${version}_${MODEL}_KSUN_${DATE}.zip"
    else
        NAME="${version}_${MODEL}_VANILLA_${DATE}.zip"
    fi

    zip -r9 "../build/out/$MODEL/$NAME" * -x ".git*" "README.md" "*placeholder" || abort
    cp -f "../build/out/$MODEL/$NAME" "$PWD/../AnyKernel3-o1s-ghost-all-in-one.zip" 2>/dev/null || true
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
