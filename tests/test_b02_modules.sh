#!/bin/bash
set -euo pipefail

echo "=== RUNNING B02 MODULE GATHERING FIXTURE TESTS ==="

FIXTURE_DIR=$(mktemp -d "/tmp/test_b02_fixture.XXXXXX")
trap 'rm -rf "$FIXTURE_DIR"' EXIT INT TERM

# Reusable module gathering function matching build.sh
gather_modules() {
    local kernel_dir="$1"
    local dest_dir="$2"
    local manifest_file="$dest_dir/modules_manifest.txt"

    rm -rf "$dest_dir"
    mkdir -p "$dest_dir"
    : > "$manifest_file"

    declare -A seen_modules
    local collision=0

    while IFS= read -r mod_path; do
        [ -z "$mod_path" ] && continue
        local mod_base
        mod_base=$(basename "$mod_path")

        # Handle known duplicate isg5320a.ko: drivers/sensors is official, sensors_lego is alternate
        if [ "$mod_base" = "isg5320a.ko" ] && [[ "$mod_path" =~ "sensors_lego" ]]; then
            echo "NOTICE: Resolved known module collision for '$mod_base': selecting 'drivers/sensors' over 'sensors_lego'."
            continue
        fi

        if [ -n "${seen_modules[$mod_base]:-}" ]; then
            echo "ERROR: Unresolved duplicate module basename collision detected: $mod_base" >&2
            echo "  Existing: ${seen_modules[$mod_base]}" >&2
            echo "  Conflict: $mod_path" >&2
            collision=1
        else
            seen_modules["$mod_base"]="$mod_path"
            cp "$mod_path" "$dest_dir/$mod_base"
            local mod_sha
            mod_sha=$(sha256sum "$mod_path" | cut -d ' ' -f 1)
            echo "$mod_sha  $mod_base  ($mod_path)" >> "$manifest_file"
        fi
    done < <(find "$kernel_dir/kernel" -type f -name '*.ko' | LC_ALL=C sort)

    if [ "$collision" -ne 0 ]; then
        echo "FATAL: Module name collision in kernel tree!" >&2
        return 1
    fi
    return 0
}

# TEST 1: Normal clean collection
echo "--- Test 1: Clean collection without collisions ---"
MOCK_KDIR="$FIXTURE_DIR/t1_kdir"
MOCK_DEST="$FIXTURE_DIR/t1_dest"
mkdir -p "$MOCK_KDIR/kernel/net" "$MOCK_KDIR/kernel/drivers/soc"
echo "module_net_content" > "$MOCK_KDIR/kernel/net/net_driver.ko"
echo "module_soc_content" > "$MOCK_KDIR/kernel/drivers/soc/soc_driver.ko"

gather_modules "$MOCK_KDIR" "$MOCK_DEST"
[ -f "$MOCK_DEST/net_driver.ko" ] || { echo "FAIL: net_driver.ko missing"; exit 1; }
[ -f "$MOCK_DEST/soc_driver.ko" ] || { echo "FAIL: soc_driver.ko missing"; exit 1; }
[ -f "$MOCK_DEST/modules_manifest.txt" ] || { echo "FAIL: manifest missing"; exit 1; }
grep -q "net_driver.ko" "$MOCK_DEST/modules_manifest.txt" || { echo "FAIL: net_driver not in manifest"; exit 1; }
echo "Test 1 PASS: Clean collection succeeds and creates manifest."

# TEST 2: Deterministic resolution of known isg5320a.ko duplicate
echo "--- Test 2: Deterministic resolution of known isg5320a.ko duplicate ---"
MOCK_KDIR_KNOWN="$FIXTURE_DIR/t2_kdir"
MOCK_DEST_KNOWN="$FIXTURE_DIR/t2_dest"
mkdir -p "$MOCK_KDIR_KNOWN/kernel/drivers/sensors" "$MOCK_KDIR_KNOWN/kernel/drivers/sensors_lego"
echo "sensors_official_content" > "$MOCK_KDIR_KNOWN/kernel/drivers/sensors/isg5320a.ko"
echo "sensors_lego_alternate" > "$MOCK_KDIR_KNOWN/kernel/drivers/sensors_lego/isg5320a.ko"

gather_modules "$MOCK_KDIR_KNOWN" "$MOCK_DEST_KNOWN"
[ -f "$MOCK_DEST_KNOWN/isg5320a.ko" ] || { echo "FAIL: isg5320a.ko missing"; exit 1; }
[ "$(cat "$MOCK_DEST_KNOWN/isg5320a.ko")" = "sensors_official_content" ] || { echo "FAIL: wrong module won"; exit 1; }
echo "Test 2 PASS: Resolved known collision deterministically to official driver."

# TEST 3: Unknown collision detection
echo "--- Test 3: Detection of unexpected collision ---"
MOCK_KDIR_COL="$FIXTURE_DIR/t3_kdir"
MOCK_DEST_COL="$FIXTURE_DIR/t3_dest"
mkdir -p "$MOCK_KDIR_COL/kernel/drivers/area_a" "$MOCK_KDIR_COL/kernel/drivers/area_b"
echo "area_a_content" > "$MOCK_KDIR_COL/kernel/drivers/area_a/other_duplicate.ko"
echo "area_b_content" > "$MOCK_KDIR_COL/kernel/drivers/area_b/other_duplicate.ko"

if gather_modules "$MOCK_KDIR_COL" "$MOCK_DEST_COL" 2>/dev/null; then
    echo "FAIL: gather_modules should have failed on unexpected collision but succeeded!"
    exit 1
else
    echo "Test 3 PASS: Correctly detected unexpected collision and returned non-zero exit code."
fi

# TEST 4: Stale module cleanup in destination
echo "--- Test 4: Stale module cleanup in destination ---"
MOCK_DEST_STALE="$FIXTURE_DIR/t4_dest"
mkdir -p "$MOCK_DEST_STALE"
echo "stale_old_module" > "$MOCK_DEST_STALE/old_stale_module.ko"

gather_modules "$MOCK_KDIR" "$MOCK_DEST_STALE"
if [ -f "$MOCK_DEST_STALE/old_stale_module.ko" ]; then
    echo "FAIL: Stale module still exists in destination!"
    exit 1
fi
echo "Test 4 PASS: Stale modules in destination are cleaned."

echo "=== ALL B02 FIXTURE TESTS PASSED! ==="
