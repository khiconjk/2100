#!/usr/bin/env bash
# Run from the host after flashing and booting an o1s build.
set -euo pipefail

adb_bin=${ADB:-adb}
serial=
output=ghost-uptime-device-check.log
max_boot_age_secs=${MAX_BOOT_AGE_SECS:-86400}
expected_ksu_version_code=${EXPECTED_KSU_VERSION_CODE:-33129}
expected_realtime_mode=${EXPECTED_REALTIME_MODE:-}
audit_packages=()

usage() {
    cat <<'EOF'
Usage: check_ghost_uptime_device.sh [--serial SERIAL] [--output FILE] \
  [--expected-ksu-version-code CODE] [--expected-realtime-mode MODE] \
  [--package PACKAGE]

The device must be booted with the V100 kernel. This is a read-only time-surface
audit. Root is used only for the status endpoint and protected partition roots.
EOF
}

while (($#)); do
    case "$1" in
        --serial)
            serial=$2
            shift 2
            ;;
        --output)
            output=$2
            shift 2
            ;;
        --max-boot-age-secs)
            max_boot_age_secs=$2
            shift 2
            ;;
        --expected-ksu-version-code)
            expected_ksu_version_code=$2
            shift 2
            ;;
        --expected-realtime-mode)
            expected_realtime_mode=$2
            shift 2
            ;;
        --package)
            audit_packages+=("$2")
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

adb_run() {
    if [[ -n "$serial" ]]; then
        "$adb_bin" -s "$serial" "$@"
    else
        "$adb_bin" "$@"
    fi
}

# Pass the complete command as one su -c argument through adb shell quoting.
adb_su() {
    local command=$1

    adb_run shell "su -c $(printf '%q' "$command")"
}

mkdir -p "$(dirname -- "$output")"
if [[ -z "${GHOST_UPTIME_TEE_ACTIVE:-}" ]]; then
    rerun_args=(--output "$output" --max-boot-age-secs "$max_boot_age_secs")
    rerun_args+=(--expected-ksu-version-code "$expected_ksu_version_code")
    if [[ -n "$expected_realtime_mode" ]]; then
        rerun_args+=(--expected-realtime-mode "$expected_realtime_mode")
    fi
    for audit_package in "${audit_packages[@]}"; do
        rerun_args+=(--package "$audit_package")
    done
    if [[ -n "$serial" ]]; then
        rerun_args+=(--serial "$serial")
    fi
    set +e
    GHOST_UPTIME_TEE_ACTIVE=1 bash "$0" "${rerun_args[@]}" 2>&1 | tee "$output"
    device_status=${PIPESTATUS[0]}
    set -e
    exit "$device_status"
fi

failures=0

pass() {
    printf 'CHECK=PASS NAME=%s\n' "$1"
}

fail() {
    printf 'CHECK=FAIL NAME=%s DETAIL=%s\n' "$1" "$2"
    failures=$((failures + 1))
}

warn() {
    printf 'CHECK=WARN NAME=%s DETAIL=%s\n' "$1" "$2"
}

record() {
    local name=$1
    shift
    local value
    value=$("$@" 2>&1 | tr -d '\r' || true)
    printf 'OBSERVED_%s=%s\n' "$name" "$value"
}

printf 'REPORT_SCHEMA=ghost-uptime-device-check-v7\n'
printf 'EXPECTED_DEVICE=o1s\n'
printf 'EXPECTED_MODE=session\n'
printf 'EXPECTED_OFFSET_RANGE_DAYS=15..25\n'
printf 'COVERAGE_AUDIT_SCHEMA=v3\n'
printf 'TIME_SURFACE_AUDIT_SCHEMA=v4\n'
printf 'EXPECTED_REALTIME_POLICY=opt-in\n'
printf 'EXPECTED_REALTIME_MODE=%s\n' "${expected_realtime_mode:-any}"
printf 'EXPECTED_KSU_VERSION_CODE=%s\n' "$expected_ksu_version_code"
printf 'EXPECTED_RTC_POLICY=unchanged\n'
printf 'EXPECTED_MONOTONIC_POLICY=session-offset\n'
printf 'EXPECTED_BOOTTIME_POLICY=session-offset\n'
printf 'EXPECTED_VDSO_POLICY=shared-timekeeper\n'
printf 'EXPECTED_PACKAGE_AGE_POLICY=unchanged\n'
printf 'EXPECTED_SERVER_TIME_POLICY=not-controlled\n'
printf 'EXPECTED_PARTITION_STAT_SCOPE=roots-only\n'
printf 'EXPECTED_DATA_PARTITION=f2fs:sda34\n'
printf 'EXPECTED_METADATA_PARTITION=ext4:sda25\n'
printf 'EXPECTED_PERSIST_PARTITION=absent\n'
printf 'RAW_CLOCK_POLICY=unchanged\n'
printf 'EXPECTED_STATUS_ENDPOINT=/proc/ghost_uptime\n'

if ! command -v "$adb_bin" >/dev/null 2>&1; then
    fail 'adb-available' "not-found:$adb_bin"
    printf 'RUNTIME_CHECK_RESULT=FAIL FAILURES=%d\n' "$failures"
    exit 1
fi

adb_run wait-for-device
pass 'adb-connected'

record KERNEL_RELEASE adb_run shell uname -r
record BUILD_FINGERPRINT adb_run shell getprop ro.build.fingerprint
record UPTIME_TEXT adb_run shell uptime
realtime_date=$(adb_run shell "date '+%s %Y-%m-%dT%H:%M:%S%z'" 2>&1 |
    tr -d '\r' || true)
printf 'OBSERVED_REALTIME_DATE=%s\n' "$realtime_date"
record PROC_UPTIME_RAW adb_run shell cat /proc/uptime
record PROC_BTIME_RAW adb_run shell "awk '/^btime / { print \$2 }' /proc/stat"
power_audit=$(adb_run shell \
    "dumpsys power | grep -E 'mWakefulness=|mLastWakeTime=|mLastSleepTime=|mHolding.*SuspendBlocker'" \
    2>&1 | tr -d '\r' || true)
printf 'OBSERVED_POWER_AUDIT_BEGIN\n%s\nOBSERVED_POWER_AUDIT_END\n' "$power_audit"
cpuidle_audit=$(adb_run shell \
    'for d in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
         printf "%s name=" "$d";
         cat "$d/name" 2>/dev/null;
         printf " time=";
         cat "$d/time" 2>/dev/null;
         printf " usage=";
         cat "$d/usage" 2>/dev/null;
         printf "\n";
     done' 2>&1 | tr -d '\r' || true)
printf 'OBSERVED_CPUIDLE_AUDIT_BEGIN\n%s\nOBSERVED_CPUIDLE_AUDIT_END\n' "$cpuidle_audit"

printf 'PACKAGE_AUDIT_BEGIN\n'
for audit_package in "${audit_packages[@]}"; do
    package_info=$(adb_run shell dumpsys package "$audit_package" 2>&1 |
        grep -E 'firstInstallTime=|lastUpdateTime=' | head -n 2 |
        tr '\r\n' ' ' | sed 's/[[:space:]]\{2,\}/ /g' || true)
    printf 'AUDIT_PACKAGE=%s INFO=%s\n' "$audit_package" "$package_info"
done
printf 'PACKAGE_AUDIT_END\n'

ksu_version_code=$(adb_run shell su -V 2>&1 | tr -d '\r' || true)
ksu_version_name=$(adb_run shell su -v 2>&1 | tr -d '\r' || true)
ksu_kernel_version=$(adb_su '/data/adb/ksud debug version' 2>&1 | tr -d '\r' || true)
printf 'OBSERVED_KSU_VERSION_CODE=%s\n' "$ksu_version_code"
printf 'OBSERVED_KSU_VERSION_NAME=%s\n' "$ksu_version_name"
printf 'OBSERVED_KSU_KERNEL_VERSION=%s\n' "$ksu_kernel_version"
printf 'EXPECTED_KSU_VERSION_CODE=%s\n' "$expected_ksu_version_code"
if [[ "$ksu_version_code" =~ ^[1-9][0-9]*$ ]]; then
    pass 'ksu-version-code'
else
    fail 'ksu-version-code' "value:$ksu_version_code"
fi
if [[ -n "$ksu_version_name" && "$ksu_version_name" != *0.0.0* ]]; then
    pass 'ksu-version-name'
else
    fail 'ksu-version-name' "value:$ksu_version_name"
fi
ksu_kernel_version_code=$(printf '%s\n' "$ksu_kernel_version" |
    awk -F': ' '$1 == "Kernel Version" { print $2; exit }')
printf 'OBSERVED_KSU_KERNEL_VERSION_CODE=%s\n' "$ksu_kernel_version_code"
if [[ "$ksu_kernel_version_code" == "$expected_ksu_version_code" ]]; then
    pass 'ksu-version-code-exact'
else
    fail 'ksu-version-code-exact' \
        "kernel-value:${ksu_kernel_version_code:-missing} expected:$expected_ksu_version_code"
fi
if [[ "$ksu_version_code" == "$ksu_kernel_version_code" ]]; then
    pass 'ksu-userspace-kernel-version-parity'
else
    warn 'ksu-userspace-kernel-version-parity' \
        "su-value:${ksu_version_code:-missing} kernel-value:${ksu_kernel_version_code:-missing}"
fi

ghost_status=$(adb_su 'cat /proc/ghost_uptime' 2>&1 | tr -d '\r' || true)
printf 'OBSERVED_GHOST_STATUS_BEGIN\n%s\nOBSERVED_GHOST_STATUS_END\n' "$ghost_status"
ghost_schema=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "schema" { print $2; exit }')
time_sync_audit_schema=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "time_sync_audit_schema" { print $2; exit }')
time_sync_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "time_sync_policy" { print $2; exit }')
adjtimex_call_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "adjtimex_call_count" { print $2; exit }')
adjtimex_setoffset_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "adjtimex_setoffset_count" { print $2; exit }')
adjtimex_frequency_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "adjtimex_frequency_count" { print $2; exit }')
timekeeping_inject_offset_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "timekeeping_inject_offset_count" { print $2; exit }')
timekeeping_inject_offset_success_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "timekeeping_inject_offset_success_count" { print $2; exit }')
timekeeping_inject_offset_failure_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "timekeeping_inject_offset_failure_count" { print $2; exit }')
ghost_mode=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "mode" { print $2; exit }')
ghost_offset_secs=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "offset_secs" { print $2; exit }')
ghost_range_days=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "range_days" { print $2; exit }')
ghost_ready=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "ready" { print $2; exit }')
time_surface_audit_schema=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "time_surface_audit_schema" { print $2; exit }')
realtime_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_policy" { print $2; exit }')
realtime_mode=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_mode" { print $2; exit }')
realtime_source=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_source" { print $2; exit }')
realtime_applied=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_applied" { print $2; exit }')
realtime_set_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_set_policy" { print $2; exit }')
realtime_set_hook_count=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "realtime_set_hook_count" { print $2; exit }')
rtc_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "rtc_policy" { print $2; exit }')
rtc_writeback_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "rtc_writeback_policy" { print $2; exit }')
monotonic_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "monotonic_policy" { print $2; exit }')
boottime_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "boottime_policy" { print $2; exit }')
proc_uptime_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "proc_uptime_policy" { print $2; exit }')
proc_btime_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "proc_btime_policy" { print $2; exit }')
vdso_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "vdso_policy" { print $2; exit }')
package_age_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "package_age_policy" { print $2; exit }')
server_time_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "server_time_policy" { print $2; exit }')
partition_stat_scope=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "partition_stat_scope" { print $2; exit }')
partition_btime_policy=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "partition_btime_policy" { print $2; exit }')
data_partition=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "data_partition" { print $2; exit }')
metadata_partition=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "metadata_partition" { print $2; exit }')
persist_partition=$(printf '%s\n' "$ghost_status" | awk -F= '$1 == "persist_partition" { print $2; exit }')

if [[ "$ghost_schema" == 'v7' && "$ghost_mode" == 'session' && \
      "$ghost_range_days" == '15..25' && "$ghost_ready" == '1' && \
      "$ghost_offset_secs" =~ ^[0-9]+$ ]]; then
    min_offset_secs=$((15 * 86400))
    max_offset_secs=$((25 * 86400))
    if (( ghost_offset_secs >= min_offset_secs && ghost_offset_secs <= max_offset_secs )); then
        pass 'ghost-status-endpoint'
    else
        fail 'ghost-status-offset-range' \
            "value:$ghost_offset_secs expected:$min_offset_secs..$max_offset_secs"
    fi
elif [[ "$ghost_schema" =~ ^v[23456]$ && "$ghost_mode" == 'session' && \
       "$ghost_range_days" == '15..25' && "$ghost_ready" == '1' && \
       "$ghost_offset_secs" =~ ^[0-9]+$ ]]; then
    min_offset_secs=$((15 * 86400))
    max_offset_secs=$((25 * 86400))
    if (( ghost_offset_secs >= min_offset_secs && ghost_offset_secs <= max_offset_secs )); then
        warn 'ghost-status-endpoint' 'legacy-schema-v6-or-older; flash-v100-for-schema-v7'
    else
        fail 'ghost-status-offset-range' \
            "value:$ghost_offset_secs expected:$min_offset_secs..$max_offset_secs"
    fi
else
    fail 'ghost-status-endpoint' 'missing-or-invalid'
fi

if [[ "$time_surface_audit_schema" == 'v4' && \
      "$realtime_policy" == 'opt-in' && \
      "$realtime_mode" =~ ^(off|backward|forward)$ && \
      "$realtime_source" =~ ^(cmdline|compiletime-default)$ && \
      "$realtime_applied" =~ ^[01]$ && \
      "$realtime_set_policy" == 'shift-absolute' && \
      "$realtime_set_hook_count" =~ ^[0-9]+$ && \
      "$rtc_policy" == 'unchanged' && \
      "$rtc_writeback_policy" == 'restore-real' && \
      "$monotonic_policy" == 'session-offset' && \
      "$boottime_policy" == 'session-offset' && \
      "$proc_uptime_policy" == 'session-offset' && \
      "$proc_btime_policy" == 'shifted-realtime-derived' && \
      "$vdso_policy" == 'shared-timekeeper' && \
      "$package_age_policy" == 'unchanged' && \
      "$server_time_policy" == 'not-controlled' ]]; then
    pass 'time-surface-policy'
elif [[ "$ghost_schema" =~ ^v[2-6]$ ]]; then
    warn 'time-surface-policy' 'legacy-schema-v6-or-older; flash-v100-for-time-sync-attestation'
else
    fail 'time-surface-policy' 'missing-or-invalid'
fi
warn 'vdso-direct-probe' 'not-available-from-shell-audit'
if [[ "$time_sync_audit_schema" == 'v1' && "$time_sync_policy" == 'observe-only' &&
      "$adjtimex_call_count" =~ ^[0-9]+$ &&
      "$adjtimex_setoffset_count" =~ ^[0-9]+$ &&
      "$adjtimex_frequency_count" =~ ^[0-9]+$ &&
      "$timekeeping_inject_offset_count" =~ ^[0-9]+$ &&
      "$timekeeping_inject_offset_success_count" =~ ^[0-9]+$ &&
      "$timekeeping_inject_offset_failure_count" =~ ^[0-9]+$ ]]; then
    pass 'time-sync-audit-policy'
    if (( timekeeping_inject_offset_success_count + timekeeping_inject_offset_failure_count == timekeeping_inject_offset_count )); then
        pass 'time-sync-inject-counter-consistency'
    else
        fail 'time-sync-inject-counter-consistency' \
            "total:$timekeeping_inject_offset_count success:$timekeeping_inject_offset_success_count failure:$timekeeping_inject_offset_failure_count"
    fi
    if (( adjtimex_setoffset_count <= adjtimex_call_count && adjtimex_frequency_count <= adjtimex_call_count )); then
        pass 'time-sync-adjtimex-counter-consistency'
    else
        fail 'time-sync-adjtimex-counter-consistency' \
            "calls:$adjtimex_call_count setoffset:$adjtimex_setoffset_count frequency:$adjtimex_frequency_count"
    fi
else
    fail 'time-sync-audit-policy' 'missing-or-invalid'
fi
printf 'OBSERVED_TIME_SYNC_AUDIT_SCHEMA=%s\n' "$time_sync_audit_schema"
printf 'OBSERVED_TIME_SYNC_POLICY=%s\n' "$time_sync_policy"
printf 'OBSERVED_ADJTIMEX_CALL_COUNT=%s\n' "$adjtimex_call_count"
printf 'OBSERVED_ADJTIMEX_SETOFFSET_COUNT=%s\n' "$adjtimex_setoffset_count"
printf 'OBSERVED_ADJTIMEX_FREQUENCY_COUNT=%s\n' "$adjtimex_frequency_count"
printf 'OBSERVED_TIMEKEEPING_INJECT_OFFSET_COUNT=%s\n' "$timekeeping_inject_offset_count"
printf 'OBSERVED_TIMEKEEPING_INJECT_OFFSET_SUCCESS_COUNT=%s\n' "$timekeeping_inject_offset_success_count"
printf 'OBSERVED_TIMEKEEPING_INJECT_OFFSET_FAILURE_COUNT=%s\n' "$timekeeping_inject_offset_failure_count"
printf 'OBSERVED_TIME_SURFACE_AUDIT_SCHEMA=%s\n' "$time_surface_audit_schema"
printf 'OBSERVED_REALTIME_POLICY=%s\n' "$realtime_policy"
printf 'OBSERVED_REALTIME_MODE=%s\n' "$realtime_mode"
printf 'OBSERVED_REALTIME_SOURCE=%s\n' "$realtime_source"
printf 'OBSERVED_REALTIME_APPLIED=%s\n' "$realtime_applied"
printf 'OBSERVED_REALTIME_SET_POLICY=%s\n' "$realtime_set_policy"
printf 'OBSERVED_REALTIME_SET_HOOK_COUNT=%s\n' "$realtime_set_hook_count"
if [[ -n "$expected_realtime_mode" && "$realtime_mode" != "$expected_realtime_mode" ]]; then
    fail 'realtime-mode-match' "value:${realtime_mode:-missing} expected:$expected_realtime_mode"
elif [[ -n "$expected_realtime_mode" ]]; then
    pass 'realtime-mode-match'
fi
if [[ "$ghost_schema" == 'v7' &&
      ("$realtime_mode" == 'off' && "$realtime_applied" == '0' ||
       "$realtime_mode" != 'off' && "$realtime_applied" == '1') ]]; then
    pass 'realtime-application-state'
elif [[ "$ghost_schema" =~ ^v[2-6]$ ]]; then
    warn 'realtime-application-state' 'legacy-schema-without-realtime-mode'
else
    fail 'realtime-application-state' "mode:${realtime_mode:-missing} applied:${realtime_applied:-missing}"
fi
if [[ "$ghost_schema" == 'v7' &&
      ("$realtime_mode" == 'off' && "$realtime_set_hook_count" == '0' ||
       "$realtime_mode" != 'off' && "$realtime_set_hook_count" =~ ^[1-9][0-9]*$) ]]; then
    pass 'realtime-set-hook-state'
elif [[ "$ghost_schema" =~ ^v[2-6]$ ]]; then
    warn 'realtime-set-hook-state' 'legacy-schema-without-central-time-set-hook'
else
    fail 'realtime-set-hook-state' \
        "mode:${realtime_mode:-missing} count:${realtime_set_hook_count:-missing}"
fi
printf 'OBSERVED_RTC_POLICY=%s\n' "$rtc_policy"
printf 'OBSERVED_RTC_WRITEBACK_POLICY=%s\n' "$rtc_writeback_policy"
printf 'OBSERVED_MONOTONIC_POLICY=%s\n' "$monotonic_policy"
printf 'OBSERVED_BOOTTIME_POLICY=%s\n' "$boottime_policy"
printf 'OBSERVED_PROC_UPTIME_POLICY=%s\n' "$proc_uptime_policy"
printf 'OBSERVED_PROC_BTIME_POLICY=%s\n' "$proc_btime_policy"
printf 'OBSERVED_VDSO_POLICY=%s\n' "$vdso_policy"
printf 'OBSERVED_PACKAGE_AGE_POLICY=%s\n' "$package_age_policy"
printf 'OBSERVED_SERVER_TIME_POLICY=%s\n' "$server_time_policy"

printf 'OBSERVED_PARTITION_STAT_SCOPE=%s\n' "$partition_stat_scope"
printf 'OBSERVED_PARTITION_BTIME_POLICY=%s\n' "$partition_btime_policy"
printf 'OBSERVED_DATA_PARTITION=%s\n' "$data_partition"
printf 'OBSERVED_METADATA_PARTITION=%s\n' "$metadata_partition"
printf 'OBSERVED_PERSIST_PARTITION=%s\n' "$persist_partition"
if [[ "$partition_stat_scope" == 'roots-only' && \
      "$partition_btime_policy" == 'shift-when-available' && \
      "$data_partition" == 'f2fs:sda34' && \
      "$metadata_partition" == 'ext4:sda25' && \
      "$persist_partition" == 'absent' ]]; then
    pass 'partition-stat-policy'
else
    fail 'partition-stat-policy' 'missing-or-invalid'
fi

data_mount=$(adb_su \
    "awk '\$2 == \"/data\" { print \$1 \" \" \$3; exit }' /proc/mounts" \
    | tr -d '\r' || true)
metadata_mount=$(adb_su \
    "awk '\$2 == \"/metadata\" { print \$1 \" \" \$3; exit }' /proc/mounts" \
    | tr -d '\r' || true)
persist_mount=$(adb_su \
    "awk '\$2 == \"/persist\" { print \$1 \" \" \$3; exit }' /proc/mounts" \
    | tr -d '\r' || true)
data_mount_source=${data_mount%% *}
data_mount_fs=${data_mount#* }
metadata_mount_source=${metadata_mount%% *}
metadata_mount_fs=${metadata_mount#* }
persist_mount_source=${persist_mount%% *}
persist_mount_fs=${persist_mount#* }
if [[ -z "$data_mount" ]]; then
    data_mount_source=
    data_mount_fs=
fi
if [[ -z "$metadata_mount" ]]; then
    metadata_mount_source=
    metadata_mount_fs=
fi
if [[ -z "$persist_mount" ]]; then
    persist_mount_source=
    persist_mount_fs=
fi

# /proc/mounts may retain a by-name symlink; compare the resolved block name.
data_mount_resolved=
metadata_mount_resolved=
if [[ -n "$data_mount_source" ]]; then
    data_mount_resolved=$(adb_su "readlink -f '$data_mount_source'" \
        | tr -d '\r' || true)
fi
if [[ -n "$metadata_mount_source" ]]; then
    metadata_mount_resolved=$(adb_su "readlink -f '$metadata_mount_source'" \
        | tr -d '\r' || true)
fi
printf 'OBSERVED_DATA_MOUNT=source=%s resolved=%s type=%s\n' \
    "${data_mount_source:-absent}" "${data_mount_resolved:-absent}" \
    "${data_mount_fs:-absent}"
printf 'OBSERVED_METADATA_MOUNT=source=%s resolved=%s type=%s\n' \
    "${metadata_mount_source:-absent}" "${metadata_mount_resolved:-absent}" \
    "${metadata_mount_fs:-absent}"
printf 'OBSERVED_PERSIST_MOUNT=source=%s type=%s\n' \
    "${persist_mount_source:-absent}" "${persist_mount_fs:-absent}"
if [[ "$data_mount_resolved" == '/dev/block/sda34' && "$data_mount_fs" == 'f2fs' ]]; then
    pass 'data-partition-observed'
else
    fail 'data-partition-observed' \
        "source:${data_mount_source:-absent} resolved:${data_mount_resolved:-absent} type:${data_mount_fs:-absent}"
fi
if [[ "$metadata_mount_resolved" == '/dev/block/sda25' && "$metadata_mount_fs" == 'ext4' ]]; then
    pass 'metadata-partition-observed'
else
    fail 'metadata-partition-observed' \
        "source:${metadata_mount_source:-absent} resolved:${metadata_mount_resolved:-absent} type:${metadata_mount_fs:-absent}"
fi
if [[ -z "$persist_mount_source" ]]; then
    pass 'persist-partition-absent'
else
    fail 'persist-partition-absent' "value:$persist_mount"
fi

# These are exact partition roots; do not recursively stat application files.
data_root_stat=$(adb_su \
    "stat -c '%n ino=%i atime=%X mtime=%Y ctime=%Z' /data" \
    | tr -d '\r' || true)
metadata_root_stat=$(adb_su \
    "stat -c '%n ino=%i atime=%X mtime=%Y ctime=%Z' /metadata" \
    | tr -d '\r' || true)
printf 'OBSERVED_DATA_ROOT_STAT=%s\n' "$data_root_stat"
printf 'OBSERVED_METADATA_ROOT_STAT=%s\n' "$metadata_root_stat"
if [[ "$data_root_stat" == *'/data '* && "$data_root_stat" == *'ino='* ]]; then
    pass 'data-root-stat-readable'
else
    fail 'data-root-stat-readable' "value:$data_root_stat"
fi
if [[ "$metadata_root_stat" == *'/metadata '* && "$metadata_root_stat" == *'ino='* ]]; then
    pass 'metadata-root-stat-readable'
else
    fail 'metadata-root-stat-readable' "value:$metadata_root_stat"
fi

# Re-read status after the two root stats to attest that the bounded VFS paths ran.
partition_status=$(adb_su 'cat /proc/ghost_uptime' 2>&1 | tr -d '\r' || true)
data_root_stat_seen=$(printf '%s\n' "$partition_status" | awk -F= '$1 == "data_root_stat_seen" { print $2; exit }')
metadata_root_stat_seen=$(printf '%s\n' "$partition_status" | awk -F= '$1 == "metadata_root_stat_seen" { print $2; exit }')
printf 'OBSERVED_GHOST_PARTITION_STATUS_BEGIN\n%s\nOBSERVED_GHOST_PARTITION_STATUS_END\n' "$partition_status"
printf 'OBSERVED_DATA_ROOT_STAT_SEEN=%s\n' "$data_root_stat_seen"
printf 'OBSERVED_METADATA_ROOT_STAT_SEEN=%s\n' "$metadata_root_stat_seen"
if [[ "$data_root_stat_seen" == '1' ]]; then
    pass 'data-root-stat-ghost-hook'
else
    fail 'data-root-stat-ghost-hook' "value:${data_root_stat_seen:-missing}"
fi
if [[ "$metadata_root_stat_seen" == '1' ]]; then
    pass 'metadata-root-stat-ghost-hook'
else
    fail 'metadata-root-stat-ghost-hook' "value:${metadata_root_stat_seen:-missing}"
fi
warn 'partition-root-btime-runtime' 'toybox-statx-btime-unavailable'

uptime_raw=$(adb_run shell cat /proc/uptime | tr -d '\r')
printf 'OBSERVED_PROC_UPTIME=%s\n' "$uptime_raw"
uptime_secs=$(printf '%s\n' "$uptime_raw" | awk '{ print $1 }')

if [[ "$uptime_secs" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    min_secs=$((15 * 86400))
    max_secs=$((25 * 86400 + max_boot_age_secs))
    if awk -v value="$uptime_secs" -v min="$min_secs" -v max="$max_secs" \
            'BEGIN { exit !(value >= min && value <= max) }'; then
        pass 'proc-uptime-range'
    else
        fail 'proc-uptime-range' "value:$uptime_secs expected:$min_secs..$max_secs"
    fi

    if [[ "$ghost_offset_secs" =~ ^[0-9]+$ ]]; then
        boot_age_secs=$(awk -v uptime="$uptime_secs" -v offset="$ghost_offset_secs" \
            'BEGIN { printf "%d", uptime - offset }')
        printf 'OBSERVED_SESSION_BOOT_AGE_SECONDS=%s\n' "$boot_age_secs"
        if [[ "$boot_age_secs" =~ ^[0-9]+$ && "$boot_age_secs" -le "$max_boot_age_secs" ]]; then
            pass 'proc-uptime-consistent-with-status'
        else
            fail 'proc-uptime-consistent-with-status' \
                "boot-age:$boot_age_secs max:$max_boot_age_secs"
        fi
    fi
else
    fail 'proc-uptime-format' "value:$uptime_secs"
fi

proc_btime=$(adb_run shell "awk '/^btime / { print \$2 }' /proc/stat" | tr -d '\r')
now_epoch=$(adb_run shell date +%s | tr -d '\r')
printf 'OBSERVED_PROC_BTIME=%s\n' "$proc_btime"
printf 'OBSERVED_REALTIME_EPOCH=%s\n' "$now_epoch"

if [[ "$proc_btime" =~ ^[0-9]+$ && "$now_epoch" =~ ^[0-9]+$ && \
      "$uptime_secs" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    uptime_int=$(printf '%s\n' "$uptime_secs" | awk -F. '{ print $1 }')
    calculated_uptime=$((now_epoch - proc_btime))
    difference=$((calculated_uptime - uptime_int))
    absolute_difference=$(printf '%s\n' "$difference" | tr -d -)
    printf 'OBSERVED_BTIME_DERIVED_UPTIME=%s\n' "$calculated_uptime"
    printf 'OBSERVED_BTIME_UPTIME_DIFFERENCE=%s\n' "$difference"
    if (( absolute_difference <= 15 )); then
        pass 'proc-btime-consistent-with-uptime'
    else
        fail 'proc-btime-consistent-with-uptime' "difference:$difference"
    fi
else
    fail 'proc-btime-format' "btime:$proc_btime realtime:$now_epoch"
fi

pid1_start=$(adb_run shell "awk '{ print \$22 }' /proc/1/stat" | tr -d '\r')
printf 'OBSERVED_PID1_START_TICKS=%s\n' "$pid1_start"
if [[ "$pid1_start" =~ ^[0-9]+$ ]]; then
    pass 'pid1-starttime-readable'

    clock_ticks=$(adb_run shell getconf CLK_TCK | tr -d '\r')
    printf 'OBSERVED_CLOCK_TICKS_PER_SECOND=%s\n' "$clock_ticks"
    if [[ "$clock_ticks" =~ ^[0-9]+$ && "$clock_ticks" -gt 0 ]]; then
        pid1_start_secs=$((pid1_start / clock_ticks))
        printf 'OBSERVED_PID1_START_SECONDS=%s\n' "$pid1_start_secs"
        if (( pid1_start_secs <= max_boot_age_secs )); then
            pass 'pid1-starttime-session-relative'
        else
            fail 'pid1-starttime-session-relative' \
                "value:$pid1_start_secs max:$max_boot_age_secs"
        fi
    else
        fail 'clock-ticks-format' "value:$clock_ticks"
    fi
else
    fail 'pid1-starttime-readable' "value:$pid1_start"
fi

rtc_time=$(adb_su 'cat /proc/driver/rtc 2>/dev/null || true' 2>&1 |
    tr -d '\r' || true)
printf 'OBSERVED_RTC_TIME=%s\n' "$rtc_time"
if [[ "$rtc_time" == *'rtc_time'* && "$rtc_time" == *'rtc_date'* ]]; then
    pass 'rtc-readable'
else
    warn 'rtc-readable' 'missing-or-inaccessible'
fi
rtc_clock=$(printf '%s\n' "$rtc_time" |
    sed -n 's/^[[:space:]]*rtc_time[[:space:]]*:[[:space:]]*//p' | head -n 1)
rtc_date=$(printf '%s\n' "$rtc_time" |
    sed -n 's/^[[:space:]]*rtc_date[[:space:]]*:[[:space:]]*//p' | head -n 1)
if [[ "$rtc_clock" =~ ^[0-9]{2}:[0-9]{2}:[0-9]{2}$ &&
      "$rtc_date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ &&
      "$now_epoch" =~ ^[0-9]+$ ]]; then
    rtc_epoch=$(date -u -d "$rtc_date $rtc_clock" +%s 2>/dev/null || true)
    if [[ "$rtc_epoch" =~ ^[0-9]+$ ]]; then
        realtime_rtc_delta=$((now_epoch - rtc_epoch))
        expected_realtime_delta=0
        case "$realtime_mode" in
            backward)
                expected_realtime_delta=$((-ghost_offset_secs))
                ;;
            forward)
                expected_realtime_delta=$ghost_offset_secs
                ;;
        esac
        realtime_rtc_error=$((realtime_rtc_delta - expected_realtime_delta))
        realtime_rtc_abs_error=${realtime_rtc_error#-}
        printf 'OBSERVED_REALTIME_RTC_DELTA_SECONDS=%s\n' \
            "$realtime_rtc_delta"
        printf 'EXPECTED_REALTIME_RTC_DELTA_SECONDS=%s\n' \
            "$expected_realtime_delta"
        printf 'OBSERVED_REALTIME_RTC_ERROR_SECONDS=%s\n' \
            "$realtime_rtc_error"
        if (( realtime_rtc_abs_error <= 90 )); then
            pass 'realtime-rtc-offset'
        else
            fail 'realtime-rtc-offset' \
                "observed:$realtime_rtc_delta expected:$expected_realtime_delta error:$realtime_rtc_error"
        fi
    else
        warn 'realtime-rtc-offset' 'host-date-parser-unavailable'
    fi
else
    warn 'realtime-rtc-offset' 'rtc-format-unavailable'
fi
boot_marker=$(adb_su "dmesg | grep -E 'ghost_uptime: schema=v[234567] mode=session' | tail -n 1" \
    | tr -d '\r' || true)
printf 'OBSERVED_GHOST_BOOT_MARKER=%s\n' "$boot_marker"
if [[ "$boot_marker" == *'ghost_uptime: schema=v2 mode=session'* ||
      "$boot_marker" == *'ghost_uptime: schema=v3 mode=session'* ||
      "$boot_marker" == *'ghost_uptime: schema=v4 mode=session'* ||
      "$boot_marker" == *'ghost_uptime: schema=v5 mode=session'* ||
      "$boot_marker" == *'ghost_uptime: schema=v6 mode=session'* ||
      "$boot_marker" == *'ghost_uptime: schema=v7 mode=session'* ]]; then
    pass 'kernel-boot-marker'
else
    warn 'kernel-boot-marker' 'missing-from-ring-buffer-or-root-unavailable'
fi

if (( failures > 0 )); then
    printf 'RUNTIME_CHECK_RESULT=FAIL FAILURES=%d\n' "$failures"
    exit 1
fi

printf 'RUNTIME_CHECK_RESULT=PASS FAILURES=0\n'
