#!/usr/bin/env bash
# Produce a concise artifact suitable for sharing with a coding assistant.
set -euo pipefail

output=ghost-uptime-report.txt
build_log=
static_log=
runtime_log=
config_file=

usage() {
    cat <<'EOF'
Usage: make_ghost_uptime_report.sh --output FILE [options]
  --build-log FILE
  --static-log FILE
  --runtime-log FILE
  --config FILE
EOF
}

while (($#)); do
    case "$1" in
        --output)
            output=$2
            shift 2
            ;;
        --build-log)
            build_log=$2
            shift 2
            ;;
        --static-log)
            static_log=$2
            shift 2
            ;;
        --runtime-log)
            runtime_log=$2
            shift 2
            ;;
        --config)
            config_file=$2
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

mkdir -p "$(dirname -- "$output")"

first_error=
if [[ -n "$build_log" && -f "$build_log" ]]; then
    first_error=$(grep -En \
        'fatal error:|(^|[[:space:]])error:|ld\.lld: error:|undefined reference|FAILED|Kernel compilation failed|Traceback|(^|[[:space:]])Exception([[:space:]:]|$)' \
        "$build_log" | head -n 1 || true)
fi

{
    printf 'REPORT_SCHEMA=ghost-uptime-report-v7\n'
    printf 'REPORT_CREATED_UTC=%s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
    printf 'GITHUB_REPOSITORY=%s\n' "${GITHUB_REPOSITORY:-local}"
    printf 'GITHUB_SHA=%s\n' "${GITHUB_SHA:-unknown}"
    printf 'WORKFLOW_RUN=%s\n' "${GITHUB_RUN_ID:-local}"

    if [[ -n "$config_file" && -f "$config_file" ]]; then
        printf '\n===== CONFIGURATION =====\n'
        cat "$config_file"
    fi

    printf '\n===== SOURCE IDENTIFIERS =====\n'
    for path in include/linux/ghost_uptime.h kernel/ghost_uptime.c \
                kernel/time/timekeeping.c drivers/rtc/hctosys.c \
                drivers/rtc/systohc.c fs/stat.c fs/proc/array.c; do
        if [[ -f "$path" ]]; then
            name=$(printf '%s' "$path" | tr '/.' '__')
            hash=$(sha256sum "$path" | awk '{ print $1 }')
            printf 'SOURCE_SHA256_%s=%s\n' "$name" "$hash"
        fi
    done

    if [[ -n "$static_log" && -f "$static_log" ]]; then
        printf '\n===== STATIC CHECK =====\n'
        grep -E '^(CHECK=|STATIC_CHECK_RESULT=)' "$static_log" || true
    fi

    printf '\n===== PARTITION COVERAGE POLICY =====\n'
    if [[ -n "$config_file" && -f "$config_file" ]]; then
        grep -E '^GHOST_(PARTITION_STAT_SCOPE|PARTITION_BTIME_POLICY|DATA_PARTITION|METADATA_PARTITION|PERSIST_PARTITION)=' \
            "$config_file" || true
    else
        printf 'PARTITION_POLICY=NOT_AVAILABLE\n'
    fi

    printf '\n===== TIME-SURFACE AUDIT POLICY =====\n'
    if [[ -n "$config_file" && -f "$config_file" ]]; then
        grep -E '^(TIME_SURFACE_AUDIT_SCHEMA|TIME_SYNC_AUDIT_SCHEMA|TIME_SYNC_POLICY|GHOST_(RTC_WRITEBACK|REALTIME(_SET)?|MONOTONIC|BOOTTIME|VDSO|PACKAGE_AGE|SERVER_TIME)_POLICY|GHOST_REALTIME_MODE|GHOST_REALTIME_DEFAULT_MODE|GHOST_REALTIME_COMPILETIME_DEFAULT_MODE|GHOST_REALTIME_SOURCE_POLICY)=' \
            "$config_file" || true
    else
        printf 'TIME_SURFACE_AUDIT=NOT_AVAILABLE\n'
    fi

    printf '\n===== KERNELSU METADATA =====\n'
    if [[ -n "$build_log" && -f "$build_log" ]]; then
        grep -E '^(KSU_SOURCE_COMMIT|KSU_VERSION_TAG|KSU_COMPUTED_VERSION_CODE|KSU_VERSION_OVERRIDE|KSU_VERSION_CODE)=' \
            "$build_log" | tail -n 5 || true
    else
        printf 'KSU_METADATA=NOT_AVAILABLE\n'
    fi

    printf '\n===== BUILD RESULT =====\n'
    if [[ -n "$build_log" && -f "$build_log" ]]; then
        grep -E 'BUILD_(START|END|EXIT_CODE|RESULT)=' "$build_log" | tail -n 20 || true
        if [[ -n "$first_error" ]]; then
            printf 'FIRST_ERROR=%s\n' "$first_error"
        else
            printf 'FIRST_ERROR=none\n'
        fi
        printf 'BUILD_LOG_LAST_80_LINES_BEGIN\n'
        tail -n 80 "$build_log" || true
        printf 'BUILD_LOG_LAST_80_LINES_END\n'
    else
        printf 'BUILD_RESULT=NOT_AVAILABLE\n'
    fi

    if [[ -n "$runtime_log" && -f "$runtime_log" ]]; then
        printf '\n===== RUNTIME CHECK =====\n'
        grep -E '^(CHECK=|OBSERVED_|AUDIT_PACKAGE=|RUNTIME_CHECK_RESULT=|REPORT_SCHEMA=|EXPECTED_)' \
            "$runtime_log" || true
    else
        printf '\nRUNTIME_CHECK_RESULT=NOT_RUN\n'
    fi
} > "$output"

printf 'CHATGPT_REPORT=%s\n' "$output"
