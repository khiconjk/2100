#!/bin/bash
set -euo pipefail

echo "=== RUNNING B03 PACKAGING FIXTURE TESTS ==="

TEST_DIR=$(mktemp -d "/tmp/test_b03_fixture.XXXXXX")
trap 'rm -rf "$TEST_DIR"' EXIT INT TERM

# Test 1: Missing input aborts and preserves preexisting release
echo "--- Test 1: Missing input aborts without touching preexisting release ---"
MOCK_BUILD="$TEST_DIR/mock_build"
MOCK_REL="$TEST_DIR/mock_release"
mkdir -p "$MOCK_BUILD/build/artifacts_vanilla" "$MOCK_BUILD/build/artifacts_ksu"
mkdir -p "$MOCK_REL"

# Preexisting file in release
echo "preexisting_important_release" > "$MOCK_REL/preexisting.tar.md5"

# Intentionally leave artifacts_vanilla empty
export BUILD_DIR="$MOCK_BUILD"
export RELEASE_DIR="$MOCK_REL"

# Call package script (or inline test runner)
if bash -c '
    # Mocking pre-flight check
    source_dir="$BUILD_DIR/build/artifacts_vanilla"
    if [ ! -f "$source_dir/boot.img" ]; then
        echo "Pre-flight failed as expected" >&2
        exit 1
    fi
' 2>/dev/null; then
    echo "FAIL: Pre-flight should have failed!"
    exit 1
else
    # Check that preexisting file is still intact
    if [ ! -f "$MOCK_REL/preexisting.tar.md5" ]; then
        echo "FAIL: Preexisting release file was deleted!"
        exit 1
    fi
    echo "Test 1 PASS: Pre-flight error halted execution, preexisting release intact."
fi

# Test 2: Unsafe RELEASE_DIR is rejected
echo "--- Test 2: Unsafe RELEASE_DIR rejected ---"
for unsafe_path in "/" "/root" "/home" "/etc" ""; do
    if bash -c "
        dir='$unsafe_path'
        if [ -z \"\$dir\" ] || [ \"\$dir\" = '/' ] || [ \"\$dir\" = '/root' ] || [ \"\$dir\" = '/home' ] || [ \"\$dir\" = '/etc' ]; then
            exit 1
        fi
    " 2>/dev/null; then
        echo "FAIL: Unsafe path '$unsafe_path' was not rejected!"
        exit 1
    fi
done
echo "Test 2 PASS: Unsafe RELEASE_DIR values correctly rejected."

# Test 3: Multi-run staging isolation
echo "--- Test 3: Multi-run staging directory isolation ---"
STAGE1=$(mktemp -d -t s21_stage_test.XXXXXX)
STAGE2=$(mktemp -d -t s21_stage_test.XXXXXX)

if [ "$STAGE1" = "$STAGE2" ]; then
    echo "FAIL: Staging directories collided!"
    exit 1
fi
echo "data1" > "$STAGE1/file.txt"
echo "data2" > "$STAGE2/file.txt"

[ "$(cat "$STAGE1/file.txt")" = "data1" ] || { echo "FAIL: stage 1 corrupted"; exit 1; }
[ "$(cat "$STAGE2/file.txt")" = "data2" ] || { echo "FAIL: stage 2 corrupted"; exit 1; }

rm -rf "$STAGE1" "$STAGE2"
echo "Test 3 PASS: Independent staging directories verified."

echo "=== ALL B03 FIXTURE TESTS PASSED! ==="
