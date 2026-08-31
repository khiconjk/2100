#!/usr/bin/env bash
# Generate only build metadata. Ghost uptime source is never rewritten by CI.
set -euo pipefail

output_dir=${1:-artifacts}
device=${DEVICE:-o1s}
age_mode=${GHOST_DEVICE_AGE_MODE:-session}
min_days=${GHOST_UPTIME_MIN_DAYS:-15}
max_days=${GHOST_UPTIME_MAX_DAYS:-25}
ksu_source_commit=${KSU_EXPECTED_SOURCE_COMMIT:-c49a6316c556f84b8e21ef3af3e1b49032b47ea0}
ksu_version_tag=${KSU_EXPECTED_VERSION_TAG:-v3.2.0-legacy}
ksu_version_code=${KSU_EXPECTED_VERSION_CODE:-33129}
ksu_version_override=${KSU_VERSION_OVERRIDE:-$ksu_version_code}
realtime_mode=${GHOST_REALTIME_MODE:-off}

if [[ "$device" != "o1s" ]]; then
    echo "ERROR: ghost uptime V100 supports only o1s; got $device" >&2
    exit 1
fi

if [[ "$age_mode" != "session" ]]; then
    echo "ERROR: only session device age is supported; got $age_mode" >&2
    exit 1
fi

if [[ "$min_days" != "15" || "$max_days" != "25" ]]; then
    echo "ERROR: V100 is fixed to a random 15..25 day offset" >&2
    exit 1
fi

if [[ ! "$realtime_mode" =~ ^(off|backward|forward)$ ]]; then
    echo "ERROR: invalid GHOST_REALTIME_MODE=$realtime_mode" >&2
    exit 1
fi

if [[ ! "$ksu_source_commit" =~ ^[0-9a-f]{40}$ ||
      -z "$ksu_version_tag" || ! "$ksu_version_code" =~ ^[1-9][0-9]*$ ||
      ! "$ksu_version_override" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: invalid expected KernelSU metadata" >&2
    exit 1
fi

mkdir -p "$output_dir"
cat > "$output_dir/ghost-uptime-config.env" <<EOF
REPORT_SCHEMA=ghost-uptime-config-v10
DEVICE=$device
GHOST_UPTIME_MODE=random-per-boot
GHOST_UPTIME_MIN_DAYS=$min_days
GHOST_UPTIME_MAX_DAYS=$max_days
GHOST_DEVICE_AGE_MODE=$age_mode
GHOST_PARTITION_STAT_SCOPE=roots-only
GHOST_PARTITION_BTIME_POLICY=shift-when-available
GHOST_DATA_PARTITION=f2fs:sda34
GHOST_METADATA_PARTITION=ext4:sda25
GHOST_PERSIST_PARTITION=absent
KSU_EXPECTED_SOURCE_COMMIT=$ksu_source_commit
KSU_EXPECTED_VERSION_TAG=$ksu_version_tag
KSU_EXPECTED_VERSION_CODE=$ksu_version_code
KSU_VERSION_OVERRIDE=$ksu_version_override
KSU_VERSION_CODE_POLICY=userspace-compatible-effective-code
GHOST_RTC_POLICY=unchanged
GHOST_RTC_WRITEBACK_POLICY=restore-real
GHOST_RAW_CLOCK_POLICY=unchanged
GHOST_REALTIME_POLICY=opt-in
GHOST_REALTIME_MODE=$realtime_mode
GHOST_REALTIME_DEFAULT_MODE=off
GHOST_REALTIME_COMPILETIME_DEFAULT_MODE=$realtime_mode
GHOST_REALTIME_SOURCE_POLICY=compiletime-default-or-cmdline
GHOST_MONOTONIC_POLICY=session-offset
GHOST_BOOTTIME_POLICY=session-offset
GHOST_VDSO_POLICY=shared-timekeeper
GHOST_PACKAGE_AGE_POLICY=unchanged
GHOST_SERVER_TIME_POLICY=not-controlled
GHOST_REALTIME_SET_POLICY=shift-absolute
TIME_SURFACE_AUDIT_SCHEMA=v4
TIME_SYNC_AUDIT_SCHEMA=v1
TIME_SYNC_POLICY=observe-only
EOF

cat "$output_dir/ghost-uptime-config.env"
