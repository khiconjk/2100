#!/bin/bash
set -euo pipefail

# 1. Resolve and validate BUILD_DIR
BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")" && pwd)}"
if [ ! -d "$BUILD_DIR" ]; then
    echo "ERROR: BUILD_DIR '$BUILD_DIR' does not exist!" >&2
    exit 1
fi
BUILD_DIR=$(realpath "$BUILD_DIR")

# 2. Resolve and validate RELEASE_DIR
RELEASE_DIR="${RELEASE_DIR:-$BUILD_DIR/release_packages}"
if [ -z "$RELEASE_DIR" ]; then
    echo "ERROR: RELEASE_DIR cannot be empty!" >&2
    exit 1
fi

RESOLVED_RELEASE_DIR=$(realpath -m "$RELEASE_DIR")

# Disallow root or dangerous top-level directories
if [ "$RESOLVED_RELEASE_DIR" = "/" ] || \
   [ "$RESOLVED_RELEASE_DIR" = "/root" ] || \
   [ "$RESOLVED_RELEASE_DIR" = "/home" ] || \
   [ "$RESOLVED_RELEASE_DIR" = "/etc" ] || \
   [ "$RESOLVED_RELEASE_DIR" = "/bin" ] || \
   [ "$RESOLVED_RELEASE_DIR" = "/usr" ] || \
   [ "$RESOLVED_RELEASE_DIR" = "/var" ]; then
    echo "ERROR: Unsafe RELEASE_DIR path: '$RESOLVED_RELEASE_DIR'!" >&2
    exit 1
fi

# Ensure RELEASE_DIR has at least 3 path components
IFS='/' read -ra PATH_PARTS <<< "$RESOLVED_RELEASE_DIR"
if [ "${#PATH_PARTS[@]}" -lt 3 ]; then
    echo "ERROR: RELEASE_DIR path too shallow: '$RESOLVED_RELEASE_DIR'!" >&2
    exit 1
fi

echo "=== PRE-FLIGHT INPUT VERIFICATION ==="
VANILLA_DIR="$BUILD_DIR/build/artifacts_vanilla"
KSU_DIR="$BUILD_DIR/build/artifacts_ksu"

resolve_artifact() {
    local dir="$1"
    local pattern="$2"
    local label="$3"

    shopt -s nullglob
    local matches=("$dir"/$pattern)
    shopt -u nullglob

    if [ "${#matches[@]}" -eq 0 ]; then
        echo "ERROR: Missing required $label ($pattern) in $dir" >&2
        return 1
    fi
    if [ "${#matches[@]}" -gt 1 ]; then
        echo "ERROR: Multiple matches found for $label: ${matches[*]}" >&2
        return 1
    fi
    if [ ! -s "${matches[0]}" ]; then
        echo "ERROR: $label file is empty (0 bytes): ${matches[0]}" >&2
        return 1
    fi
    echo "${matches[0]}"
}

# Pre-flight check ALL 8 inputs BEFORE modifying any destination files
V_BOOT=$(resolve_artifact "$VANILLA_DIR" "boot.img" "Vanilla boot.img")
V_VBOOT=$(resolve_artifact "$VANILLA_DIR" "vendor_boot.img" "Vanilla vendor_boot.img")
V_DTBO=$(resolve_artifact "$VANILLA_DIR" "dtbo.img" "Vanilla dtbo.img")
V_ZIP=$(resolve_artifact "$VANILLA_DIR" "*VANILLA*.zip" "Vanilla AnyKernel3 ZIP")

K_BOOT=$(resolve_artifact "$KSU_DIR" "boot.img" "KSU boot.img")
K_VBOOT=$(resolve_artifact "$KSU_DIR" "vendor_boot.img" "KSU vendor_boot.img")
K_DTBO=$(resolve_artifact "$KSU_DIR" "dtbo.img" "KSU dtbo.img")
K_ZIP=$(resolve_artifact "$KSU_DIR" "*KSUN_SUSFS*.zip" "KSU AnyKernel3 ZIP")

echo "All 8 required build inputs verified present and non-empty."

# 3. Isolated Staging Directory per Execution
STAGE_DIR=$(mktemp -d -t s21_package_stage.XXXXXX)
cleanup() {
    local exit_code=$?
    if [ -d "$STAGE_DIR" ]; then
        rm -rf "$STAGE_DIR"
    fi
    if [ $exit_code -ne 0 ]; then
        echo "ABORT: Packaging failed with exit code $exit_code. RELEASE_DIR was preserved intact." >&2
    fi
}
trap cleanup EXIT INT TERM

STAGE_VANILLA="$STAGE_DIR/vanilla"
STAGE_KSU="$STAGE_DIR/ksu"
STAGE_OUT="$STAGE_DIR/output"
mkdir -p "$STAGE_VANILLA" "$STAGE_KSU" "$STAGE_OUT"

echo "=== PACKAGING VANILLA ODIN TAR & LZ4 ==="
cp "$V_BOOT" "$STAGE_VANILLA/boot.img"
cp "$V_VBOOT" "$STAGE_VANILLA/vendor_boot.img"
cp "$V_DTBO" "$STAGE_VANILLA/dtbo.img"

# Vanilla Raw TAR
tar -H ustar -cf "$STAGE_OUT/G991B_VANILLA_ODIN.tar" -C "$STAGE_VANILLA" boot.img vendor_boot.img dtbo.img
(cd "$STAGE_OUT" && md5sum -t G991B_VANILLA_ODIN.tar >> G991B_VANILLA_ODIN.tar && mv G991B_VANILLA_ODIN.tar G991B_VANILLA_ODIN.tar.md5)

# Vanilla LZ4 TAR
lz4 -B6 --content-size -f "$STAGE_VANILLA/boot.img" "$STAGE_VANILLA/boot.img.lz4"
lz4 -B6 --content-size -f "$STAGE_VANILLA/vendor_boot.img" "$STAGE_VANILLA/vendor_boot.img.lz4"
lz4 -B6 --content-size -f "$STAGE_VANILLA/dtbo.img" "$STAGE_VANILLA/dtbo.img.lz4"
tar -H ustar -cf "$STAGE_OUT/G991B_VANILLA_ODIN_LZ4.tar" -C "$STAGE_VANILLA" boot.img.lz4 vendor_boot.img.lz4 dtbo.img.lz4
(cd "$STAGE_OUT" && md5sum -t G991B_VANILLA_ODIN_LZ4.tar >> G991B_VANILLA_ODIN_LZ4.tar && mv G991B_VANILLA_ODIN_LZ4.tar G991B_VANILLA_ODIN_LZ4.tar.md5)

# Vanilla TWRP ZIP
cp "$V_ZIP" "$STAGE_OUT/G991B_VANILLA_TWRP.zip"

echo "=== PACKAGING KSU+SUSFS ODIN TAR & LZ4 ==="
cp "$K_BOOT" "$STAGE_KSU/boot.img"
cp "$K_VBOOT" "$STAGE_KSU/vendor_boot.img"
cp "$K_DTBO" "$STAGE_KSU/dtbo.img"

# KSU Raw TAR
tar -H ustar -cf "$STAGE_OUT/G991B_ALL_KSUN_SUSFS_ODIN.tar" -C "$STAGE_KSU" boot.img vendor_boot.img dtbo.img
(cd "$STAGE_OUT" && md5sum -t G991B_ALL_KSUN_SUSFS_ODIN.tar >> G991B_ALL_KSUN_SUSFS_ODIN.tar && mv G991B_ALL_KSUN_SUSFS_ODIN.tar G991B_ALL_KSUN_SUSFS_ODIN.tar.md5)

# KSU LZ4 TAR
lz4 -B6 --content-size -f "$STAGE_KSU/boot.img" "$STAGE_KSU/boot.img.lz4"
lz4 -B6 --content-size -f "$STAGE_KSU/vendor_boot.img" "$STAGE_KSU/vendor_boot.img.lz4"
lz4 -B6 --content-size -f "$STAGE_KSU/dtbo.img" "$STAGE_KSU/dtbo.img.lz4"
tar -H ustar -cf "$STAGE_OUT/G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar" -C "$STAGE_KSU" boot.img.lz4 vendor_boot.img.lz4 dtbo.img.lz4
(cd "$STAGE_OUT" && md5sum -t G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar >> G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar && mv G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar.md5)

# KSU TWRP ZIP
cp "$K_ZIP" "$STAGE_OUT/G991B_ALL_KSUN_SUSFS_TWRP.zip"

echo "=== VERIFYING PACKAGES INSIDE STAGING BEFORE PROMOTION ==="
verify_tar_md5() {
    local fpath="$1"
    local fname
    fname=$(basename "$fpath")
    local tar_name="${fname%.md5}"
    
    local line_bytes=$((32 + 2 + ${#tar_name} + 1))
    local total_bytes
    total_bytes=$(wc -c < "$fpath")
    local raw_bytes=$((total_bytes - line_bytes))
    
    local footer
    footer=$(tail -c "$line_bytes" "$fpath")
    local expected_md5
    expected_md5=$(echo "$footer" | awk '{print $1}')
    local footer_name
    footer_name=$(echo "$footer" | awk '{print $2}')
    
    if [ "$footer_name" != "$tar_name" ]; then
        echo "ERROR: Invalid footer name in $fname: '$footer_name' != '$tar_name'" >&2
        return 1
    fi
    
    local calc_md5
    calc_md5=$(head -c "$raw_bytes" "$fpath" | md5sum | cut -d ' ' -f 1)
    if [ "$calc_md5" != "$expected_md5" ]; then
        echo "ERROR: MD5 mismatch in $fname: calculated $calc_md5 != expected $expected_md5" >&2
        return 1
    fi
    echo "  $fname: Embedded MD5 verified PASS ($expected_md5)"
}

verify_tar_md5 "$STAGE_OUT/G991B_VANILLA_ODIN.tar.md5"
verify_tar_md5 "$STAGE_OUT/G991B_VANILLA_ODIN_LZ4.tar.md5"
verify_tar_md5 "$STAGE_OUT/G991B_ALL_KSUN_SUSFS_ODIN.tar.md5"
verify_tar_md5 "$STAGE_OUT/G991B_ALL_KSUN_SUSFS_ODIN_LZ4.tar.md5"

unzip -tqq "$STAGE_OUT/G991B_VANILLA_TWRP.zip"
echo "  G991B_VANILLA_TWRP.zip: Integrity PASS"
unzip -tqq "$STAGE_OUT/G991B_ALL_KSUN_SUSFS_TWRP.zip"
echo "  G991B_ALL_KSUN_SUSFS_TWRP.zip: Integrity PASS"

# 4. Safe Promotion to RELEASE_DIR (Non-destructive with archive preservation)
echo "=== PROMOTING VERIFIED PACKAGES TO RELEASE_DIR ==="
mkdir -p "$RESOLVED_RELEASE_DIR"

BUILD_STAMP=$(date +"%Y%m%d_%H%M%S")
TIMESTAMPED_DIR="$RESOLVED_RELEASE_DIR/build_$BUILD_STAMP"
mkdir -p "$TIMESTAMPED_DIR"

# 1. Store a dedicated immutable copy in the timestamped build directory
for pkg in "$STAGE_OUT"/*; do
    cp -f "$pkg" "$TIMESTAMPED_DIR/"
done
echo "  Timestamped build archived in: $TIMESTAMPED_DIR"

# 2. Update the root release directory with latest packages
for pkg in "$STAGE_OUT"/*; do
    cp -f "$pkg" "$RESOLVED_RELEASE_DIR/"
done

echo "=== FINAL RELEASE PACKAGES IN $RESOLVED_RELEASE_DIR ==="
ls -lh "$RESOLVED_RELEASE_DIR"
