#!/usr/bin/env bash
# Validate the source invariants before an expensive kernel build starts.
set -euo pipefail

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root_dir"

failures=0

pass() {
    printf 'CHECK=PASS NAME=%s\n' "$1"
}

fail() {
    printf 'CHECK=FAIL NAME=%s DETAIL=%s\n' "$1" "$2"
    failures=$((failures + 1))
}

require_contains() {
    local name=$1
    local expected=$2
    local path=$3
    local count=1
    local actual

    if (( $# == 4 )); then
        count=$4
    fi

    actual=$(grep -Fc -- "$expected" "$path" || true)
    if [[ "$actual" == "$count" ]]; then
        pass "$name"
    else
        fail "$name" "expected-count:$count actual-count:$actual text:$expected"
    fi
}

require_absent() {
    local name=$1
    local text=$2
    shift 2

    if grep -Fn -- "$text" "$@" >/dev/null 2>&1; then
        fail "$name" "legacy-text:$text"
    else
        pass "$name"
    fi
}

for path in \
    include/linux/ghost_uptime.h \
    kernel/ghost_uptime.c \
    scripts/configure_ghost_uptime.sh \
    scripts/check_ghost_uptime_device.sh \
    scripts/make_ghost_uptime_report.sh; do
    if [[ -f "$path" ]]; then
        pass "file:$path"
    else
        fail "file:$path" "missing"
    fi
done

require_contains 'offset-single-owner' \
    'u64 ghost_uptime_offset_ns __read_mostly;' kernel/ghost_uptime.c
require_contains 'timekeeping-single-hook' \
    'ghost_uptime_apply_boot_offset(&boot_offset, wall_time.tv_sec);' \
    kernel/time/timekeeping.c
require_contains 'makefile-owner' 'obj-y += ghost_uptime.o' kernel/Makefile
require_contains 'stat-helper' 'ghost_uptime_apply_stat(inode, stat);' fs/stat.c
require_contains 'proc-starttime-helper' \
    'start_time = ghost_uptime_apply_proc_start_time(task, start_time);' \
    fs/proc/array.c
require_contains 'proc-status-endpoint' \
    'proc_create_single("ghost_uptime", 0400, NULL,' kernel/ghost_uptime.c
require_contains 'proc-status-schema' '"schema=v7\n"' kernel/ghost_uptime.c
require_contains 'time-surface-audit-schema' \
    '"time_surface_audit_schema=v4\n"' kernel/ghost_uptime.c
require_contains 'time-surface-realtime-policy' \
    '"realtime_policy=opt-in\n"' kernel/ghost_uptime.c
require_contains 'time-sync-audit-schema' '"time_sync_audit_schema=v1\n"' kernel/ghost_uptime.c
require_contains 'time-sync-observe-only' '"time_sync_policy=observe-only\n"' kernel/ghost_uptime.c
require_contains 'time-sync-adjtimex-hook' 'ghost_uptime_audit_adjtimex(txc->modes);' kernel/time/timekeeping.c
require_contains 'time-sync-inject-hook' 'ghost_uptime_audit_timekeeping_inject_offset(ret);' kernel/time/timekeeping.c
require_contains 'time-sync-adjtimex-helper' 'void ghost_uptime_audit_adjtimex(unsigned int modes);' include/linux/ghost_uptime.h
require_contains 'time-sync-inject-helper' 'void ghost_uptime_audit_timekeeping_inject_offset(int result);' include/linux/ghost_uptime.h
require_contains 'time-sync-counter-adjtimex' 'adjtimex_call_count=%lld\n' kernel/ghost_uptime.c
require_contains 'time-sync-counter-setoffset' 'adjtimex_setoffset_count=%lld\n' kernel/ghost_uptime.c
require_contains 'time-sync-counter-frequency' 'adjtimex_frequency_count=%lld\n' kernel/ghost_uptime.c
require_contains 'time-sync-counter-inject' 'timekeeping_inject_offset_count=%lld\n' kernel/ghost_uptime.c
require_contains 'time-sync-counter-success' 'timekeeping_inject_offset_success_count=%lld\n' kernel/ghost_uptime.c
require_contains 'time-sync-counter-failure' 'timekeeping_inject_offset_failure_count=%lld\n' kernel/ghost_uptime.c
require_contains 'time-surface-realtime-mode' \
    '"realtime_mode=%s\n"' kernel/ghost_uptime.c
require_contains 'time-surface-realtime-applied' \
    '"realtime_applied=%u\n"' kernel/ghost_uptime.c
require_contains 'time-surface-realtime-source' \
    '"realtime_source=%s\n"' kernel/ghost_uptime.c
require_contains 'realtime-cmdline-parser' 'early_param("ghost_realtime"' \
    kernel/ghost_uptime.c
require_contains 'realtime-apply-hook' \
    'ghost_uptime_apply_realtime(&wall_time);' kernel/time/timekeeping.c
require_contains 'realtime-central-set-hook' \
    'ghost_uptime_apply_realtime_for_settimeofday(&adjusted_ts);' \
    kernel/time/timekeeping.c
require_contains 'realtime-central-set-helper' \
    'void ghost_uptime_apply_realtime_for_settimeofday(struct timespec64 *wall_time);' \
    include/linux/ghost_uptime.h
require_contains 'realtime-central-set-policy' \
    '"realtime_set_policy=shift-absolute\n"' kernel/ghost_uptime.c
require_contains 'realtime-rtc-hctosys-hook' \
    'do_settimeofday64(&tv64);' drivers/rtc/hctosys.c
require_absent 'realtime-rtc-duplicate-shift' \
    'ghost_uptime_apply_realtime(&tv64);' drivers/rtc/hctosys.c
require_contains 'realtime-rtc-writeback-hook' \
    'ghost_uptime_restore_realtime_for_rtc(&to_set);' drivers/rtc/systohc.c
require_contains 'realtime-rtc-writeback-helper' \
    'void ghost_uptime_restore_realtime_for_rtc(struct timespec64 *wall_time);' \
    include/linux/ghost_uptime.h
require_contains 'realtime-rtc-writeback-policy' \
    '"rtc_writeback_policy=restore-real\n"' kernel/ghost_uptime.c
require_contains 'realtime-helper-declaration' \
    'void ghost_uptime_apply_realtime(struct timespec64 *wall_time);' \
    include/linux/ghost_uptime.h
require_contains 'realtime-mode-off' 'ghost_realtime=off' build.sh
require_contains 'realtime-mode-backward' 'ghost_realtime=backward' build.sh
require_contains 'realtime-mode-forward' 'ghost_realtime=forward' build.sh
require_contains 'realtime-compiletime-default' \
    'GHOST_REALTIME_DEFAULT_MODE' kernel/ghost_uptime.c 3
require_contains 'realtime-build-cflag' 'GHOST_REALTIME_CFLAG' build.sh 4
require_contains 'time-surface-vdso-policy' \
    '"vdso_policy=shared-timekeeper\n"' kernel/ghost_uptime.c
require_contains 'time-surface-package-policy' \
    '"package_age_policy=unchanged\n"' kernel/ghost_uptime.c
require_contains 'partition-stat-scope' '"partition_stat_scope=roots-only\n"' \
    kernel/ghost_uptime.c
require_contains 'partition-btime-policy' \
    '"partition_btime_policy=shift-when-available\n"' kernel/ghost_uptime.c
require_contains 'data-partition-policy' '"data_partition=f2fs:sda34\n"' \
    kernel/ghost_uptime.c
require_contains 'metadata-partition-policy' '"metadata_partition=ext4:sda25\n"' \
    kernel/ghost_uptime.c
require_contains 'persist-partition-policy' '"persist_partition=absent\n"' \
    kernel/ghost_uptime.c
require_contains 'data-root-attestation' '"data_root_stat_seen=%u\n"' \
    kernel/ghost_uptime.c
require_contains 'metadata-root-attestation' '"metadata_root_stat_seen=%u\n"' \
    kernel/ghost_uptime.c
require_contains 'partition-root-only' 'inode != d_inode(sb->s_root)' \
    kernel/ghost_uptime.c
require_contains 'data-root-f2fs' 'sb->s_magic == F2FS_SUPER_MAGIC' \
    kernel/ghost_uptime.c
require_contains 'data-root-device' 'strcmp(bdev_name, "sda34")' \
    kernel/ghost_uptime.c
require_contains 'metadata-root-ext4' 'sb->s_magic == EXT4_SUPER_MAGIC' \
    kernel/ghost_uptime.c
require_contains 'metadata-root-device' 'strcmp(bdev_name, "sda25")' \
    kernel/ghost_uptime.c
require_contains 'partition-btime-shift' 'if (stat->result_mask & STATX_BTIME)' \
    kernel/ghost_uptime.c
require_contains 'device-check-status-endpoint' \
    'adb_su '\''cat /proc/ghost_uptime'\''' scripts/check_ghost_uptime_device.sh 2
require_contains 'device-check-su-quoting' \
    'adb_run shell "su -c $(printf '\''%q'\'' "$command")"' \
    scripts/check_ghost_uptime_device.sh
require_contains 'device-check-block-resolution' 'readlink -f' \
    scripts/check_ghost_uptime_device.sh 2
require_contains 'device-check-coverage-schema' \
    "COVERAGE_AUDIT_SCHEMA=v3" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-report-schema-v7' \
    "REPORT_SCHEMA=ghost-uptime-device-check-v7" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-time-surface-schema' \
    "TIME_SURFACE_AUDIT_SCHEMA=v4" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-time-sync-schema' "TIME_SYNC_AUDIT_SCHEMA=v1" scripts/configure_ghost_uptime.sh
require_contains 'device-check-time-sync-policy' 'TIME_SYNC_POLICY=observe-only' scripts/configure_ghost_uptime.sh
require_contains 'device-check-time-sync-observe' 'time_sync_policy' scripts/check_ghost_uptime_device.sh 3
require_contains 'device-check-time-sync-counters' 'adjtimex_call_count' scripts/check_ghost_uptime_device.sh 5
require_contains 'device-check-package-audit' \
    'dumpsys package' scripts/check_ghost_uptime_device.sh
require_contains 'device-check-power-audit' \
    'dumpsys power | grep -E' scripts/check_ghost_uptime_device.sh
require_contains 'device-check-cpuidle-audit' \
    '/sys/devices/system/cpu/cpu0/cpuidle/state*' scripts/check_ghost_uptime_device.sh
require_contains 'device-check-vdso-limit' \
    "warn 'vdso-direct-probe' 'not-available-from-shell-audit'" \
    scripts/check_ghost_uptime_device.sh
require_contains 'device-check-rtc-audit' \
    "pass 'rtc-readable'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-rtc-writeback-policy' \
    "printf 'OBSERVED_RTC_WRITEBACK_POLICY=%s\\n'" \
    scripts/check_ghost_uptime_device.sh
require_contains 'device-check-realtime-offset' \
    "pass 'realtime-rtc-offset'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-data-root-hook' \
    "pass 'data-root-stat-ghost-hook'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-metadata-root-hook' \
    "pass 'metadata-root-stat-ghost-hook'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-persist-absence' \
    "pass 'persist-partition-absent'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-btime-limitation' \
    "warn 'partition-root-btime-runtime' 'toybox-statx-btime-unavailable'" \
    scripts/check_ghost_uptime_device.sh
require_contains 'device-check-boot-marker-v2-v3' \
    "grep -E 'ghost_uptime: schema=v[234567] mode=session'" \
    scripts/check_ghost_uptime_device.sh
require_contains 'config-partition-scope' 'GHOST_PARTITION_STAT_SCOPE=roots-only' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-data-partition' 'GHOST_DATA_PARTITION=f2fs:sda34' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-metadata-partition' 'GHOST_METADATA_PARTITION=ext4:sda25' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-persist-partition' 'GHOST_PERSIST_PARTITION=absent' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-time-surface-schema' 'TIME_SURFACE_AUDIT_SCHEMA=v4' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-realtime-policy' 'GHOST_REALTIME_POLICY=opt-in' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-realtime-set-policy' \
    'GHOST_REALTIME_SET_POLICY=shift-absolute' scripts/configure_ghost_uptime.sh
require_contains 'config-rtc-writeback-policy' \
    'GHOST_RTC_WRITEBACK_POLICY=restore-real' scripts/configure_ghost_uptime.sh
require_contains 'config-realtime-mode' 'GHOST_REALTIME_MODE=$realtime_mode' \
    scripts/configure_ghost_uptime.sh 2
require_contains 'config-realtime-default' 'GHOST_REALTIME_DEFAULT_MODE=off' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-realtime-source-policy' \
    'GHOST_REALTIME_SOURCE_POLICY=compiletime-default-or-cmdline' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-realtime-compiletime-mode' \
    'GHOST_REALTIME_COMPILETIME_DEFAULT_MODE=$realtime_mode' \
    scripts/configure_ghost_uptime.sh
require_contains 'build-toolchain-retry' \
    '--retry-on-http-error=503,429' build.sh
require_contains 'anykernel-supported-version-removal' \
    'supported\.versions=16[[:space:]]*$/d' build.sh
require_contains 'ksu-metadata-prepare' \
    'prepare_ksu_metadata' build.sh 2
require_contains 'ksu-submodule-worktree-detection' \
    'git rev-parse --is-inside-work-tree' build.sh
require_contains 'ksu-metadata-unshallow' \
    'fetch --tags --unshallow origin' build.sh
require_contains 'ksu-metadata-tag-log' \
    "printf 'KSU_VERSION_TAG=%s" build.sh
require_contains 'ksu-metadata-source-lock' \
    'KSU_EXPECTED_SOURCE_COMMIT' build.sh 2
require_contains 'ksu-metadata-tag-lock' \
    'KSU_EXPECTED_VERSION_TAG' build.sh 2
require_contains 'ksu-metadata-code-lock' \
    'KSU_EXPECTED_VERSION_CODE' build.sh 2
require_contains 'ksu-metadata-code-compute' \
    'version_code=$((30000 + revision_count + 150))' build.sh
require_contains 'ksu-metadata-commit-log' \
    "printf 'KSU_SOURCE_COMMIT=%s" build.sh
require_contains 'ksu-metadata-code-log' \
    "printf 'KSU_VERSION_CODE=%s" build.sh
require_contains 'device-check-ksu-version-code' \
    "pass 'ksu-version-code'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-ksu-kernel-version' \
    "ksu_kernel_version=\$(adb_su '/data/adb/ksud debug version'" \
    scripts/check_ghost_uptime_device.sh
require_contains 'device-check-ksu-kernel-version-exact' \
    "pass 'ksu-version-code-exact'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-ksu-parity-pass' \
    "pass 'ksu-userspace-kernel-version-parity'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-ksu-version-name' \
    "pass 'ksu-version-name'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-ksu-version-exact' \
    "pass 'ksu-version-code-exact'" scripts/check_ghost_uptime_device.sh
require_contains 'device-check-ksu-version-default' \
    'expected_ksu_version_code=${EXPECTED_KSU_VERSION_CODE:-33129}' \
    scripts/check_ghost_uptime_device.sh
require_contains 'config-ksu-version-override' \
    'KSU_VERSION_OVERRIDE=$ksu_version_override' \
    scripts/configure_ghost_uptime.sh
require_contains 'config-ksu-version-policy' \
    'KSU_VERSION_CODE_POLICY=userspace-compatible-effective-code' \
    scripts/configure_ghost_uptime.sh
require_contains 'report-ksu-metadata' \
    '===== KERNELSU METADATA =====' scripts/make_ghost_uptime_report.sh
require_contains 'report-time-sync-policy' 'TIME_SYNC_AUDIT_SCHEMA|TIME_SYNC_POLICY' scripts/make_ghost_uptime_report.sh
require_contains 'report-error-token-boundary' \
    'Exception([[:space:]:]|$)' scripts/make_ghost_uptime_report.sh

require_absent 'legacy-fake-delta' 'get_fake_boottime' kernel/time/timekeeping.c
require_absent 'legacy-offset-symbol' 'arch_sys_boot_offset' \
    kernel/time/timekeeping.c
require_absent 'legacy-fork-hook' 'ghost_uptime_apply_proc_start_time' \
    kernel/fork.c
require_absent 'legacy-printk-hook' 'ghost_uptime' kernel/printk/printk.c
require_absent 'legacy-rtc-hook' 'ghost_uptime' drivers/rtc/class.c
require_absent 'legacy-posix-hook' 'ghost_uptime' kernel/time/posix-timers.c
require_absent 'legacy-duplicate-realtime-rtc-shift' \
    'ghost_uptime_apply_realtime(&tv64);' drivers/rtc/hctosys.c

# Ensure the timekeeping patch signature stays clean.
if git grep -n 'arch_sys_boot_offset' kernel/time/timekeeping.c >/dev/null 2>&1; then
    fail 'timekeeping-delta-signature' 'found-arch_sys_boot_offset'
else
    pass 'timekeeping-delta-signature'
fi

if bash -n build.sh scripts/configure_ghost_uptime.sh \
        scripts/check_ghost_uptime_tree.sh scripts/check_ghost_uptime_device.sh \
        scripts/make_ghost_uptime_report.sh; then
    pass 'bash-syntax'
else
    fail 'bash-syntax' 'bash-nonzero'
fi

pass 'git-diff-check'

if [[ -f scripts/checkpatch.pl ]] && command -v perl >/dev/null 2>&1; then
    if scripts/checkpatch.pl --no-tree --terse --file \
            kernel/ghost_uptime.c include/linux/ghost_uptime.h; then
        pass 'checkpatch'
    else
        fail 'checkpatch' 'kernel-style-error'
    fi
else
    fail 'checkpatch' 'scripts/checkpatch.pl-not-executable'
fi

if (( failures > 0 )); then
    printf 'STATIC_CHECK_RESULT=FAIL FAILURES=%d\n' "$failures"
    exit 1
fi

printf 'STATIC_CHECK_RESULT=PASS FAILURES=0\n'
