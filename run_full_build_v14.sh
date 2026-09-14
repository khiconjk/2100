#!/bin/bash
set -e

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")" && pwd)}"
cd "$BUILD_DIR"

echo "=========================================================="
echo "  Ghost Kernel v14 with Widevine DRM Virtualization Patch  "
echo "=========================================================="
echo ""

# 1. Step 1: Vanilla build
echo ">>> STEP 1/2: Building VANILLA kernel..."
echo "----------------------------------------------------------"
bash build.sh -m o1s -k n -s n

# Find latest vanilla zip and extract images
mkdir -p build/artifacts_vanilla
rm -f build/artifacts_vanilla/*
LATEST_V_ZIP=$(ls -t build/out/o1s/history/*VANILLA*.zip build/out/o1s/*VANILLA*.zip 2>/dev/null | head -n 1)
if [ -z "$LATEST_V_ZIP" ]; then
    echo "ERROR: Vanilla ZIP not found!"
    exit 1
fi
echo "Found Vanilla ZIP: $LATEST_V_ZIP"
cp "$LATEST_V_ZIP" build/artifacts_vanilla/
unzip -o -q "$LATEST_V_ZIP" boot.img vendor_boot.img dtbo.img -d build/artifacts_vanilla/
if [ -f build/out/o1s/Image ]; then
    cp build/out/o1s/Image build/artifacts_vanilla/Image_v14_vanilla
fi
echo "Vanilla artifacts staged successfully."
echo ""

# 2. Step 2: KSU + SuSFS build
echo ">>> STEP 2/2: Building KSU + SuSFS kernel..."
echo "----------------------------------------------------------"
bash build.sh -m o1s -k y -s y

mkdir -p build/artifacts_ksu
rm -f build/artifacts_ksu/*
LATEST_K_ZIP=$(ls -t build/out/o1s/*KSUN_SUSFS*.zip 2>/dev/null | head -n 1)
if [ -z "$LATEST_K_ZIP" ]; then
    echo "ERROR: KSU+SuSFS ZIP not found!"
    exit 1
fi
echo "Found KSU ZIP: $LATEST_K_ZIP"
cp "$LATEST_K_ZIP" build/artifacts_ksu/
cp build/out/o1s/boot.img build/artifacts_ksu/boot.img
cp build/out/o1s/vendor_boot.img build/artifacts_ksu/vendor_boot.img
cp build/out/o1s/dtbo.img build/artifacts_ksu/dtbo.img
if [ -f build/out/o1s/Image ]; then
    cp build/out/o1s/Image build/artifacts_ksu/Image_v14_ksu
fi
echo "KSU artifacts staged successfully."
echo ""

# 3. Packaging
echo ">>> STEP 3/3: Executing package_all.sh..."
echo "----------------------------------------------------------"
export BUILD_DIR="$BUILD_DIR"
export RELEASE_DIR="${RELEASE_DIR:-$BUILD_DIR/release_packages}"
bash ./package_all.sh

echo ""
echo "=========================================================="
echo "  BUILD & PACKAGING COMPLETE!"
echo "=========================================================="
