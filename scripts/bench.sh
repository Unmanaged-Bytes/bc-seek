#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# bc-seek benchmark: compares bc-seek against find(1) and fd(1)
# on common scenarios (walk-only, name match, size filter).
#
# Default: warm-only (no sudo). --with-cold runs cold iterations
# after dropping page cache.
#
# Produces build/perf-logs/bench.log.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/build/release/src/bc-seek"
TARGET=""
WITH_COLD=0
WARM_RUNS="${WARM_RUNS:-10}"
COLD_RUNS="${COLD_RUNS:-3}"

usage() {
    cat <<EOF
usage: bench.sh [--with-cold] [--target <path>] <target-directory>

Compares bc-seek against find and fd on a target filesystem tree.
Writes build/perf-logs/bench.log.

Options:
  --with-cold    also run cold-cache iterations (requires sudo for
                 drop_caches)
  --target PATH  alternate way to pass the target directory
  -h, --help     show this help

Environment variables: WARM_RUNS (default 10), COLD_RUNS (default 3).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-cold)   WITH_COLD=1; shift ;;
        --target)      TARGET="$2"; shift 2 ;;
        -h|--help)     usage; exit 0 ;;
        *)             TARGET="$1"; shift ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "error: missing target directory" >&2
    usage >&2
    exit 2
fi

OUT="$ROOT/build/perf-logs/bench.log"

if [[ $WITH_COLD -eq 1 && "$(id -u)" -ne 0 ]]; then
    exec sudo -E bash "$0" --with-cold --target "$TARGET"
fi

[[ -x "$BIN" ]] || {
    echo "missing release binary: $BIN" >&2
    echo "build first: bc-build $ROOT release" >&2
    exit 1
}
[[ -d "$TARGET" ]] || { echo "missing target: $TARGET" >&2; exit 1; }

FIND_BIN="$(command -v find || true)"
FD_BIN="$(command -v fd 2>/dev/null || command -v fdfind 2>/dev/null || true)"

[[ -n "$FIND_BIN" ]] || { echo "error: find(1) not available" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
: > "$OUT"

log() { tee -a "$OUT"; }

GIT_REV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "=== bc-seek bench @ $GIT_REV, target=$TARGET ===" | log
echo "warm runs=$WARM_RUNS  cold=$WITH_COLD" | log
echo "env: boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo ?)  governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo ?)  ASLR=$(cat /proc/sys/kernel/randomize_va_space 2>/dev/null || echo ?)" | log
echo "tools:" | log
"$BIN" --version 2>&1 | head -1 | sed 's/^/  bc-seek: /' | log
"$FIND_BIN" --version 2>&1 | head -1 | sed 's/^/  find: /' | log
[[ -n "$FD_BIN" ]] && "$FD_BIN" --version 2>&1 | head -1 | sed 's/^/  fd: /' | log
echo "" | log

# -----------------------------------------------------------------------------
# Helper: run a command N times, report per-run wall time and stats.
# -----------------------------------------------------------------------------
run_series() {
    local label="$1"
    local runs="$2"
    shift 2
    local samples=()
    for ((i = 1; i <= runs; i++)); do
        local t1 t2
        t1=$(date +%s.%N)
        "$@" >/dev/null 2>&1
        t2=$(date +%s.%N)
        samples+=("$(awk "BEGIN { printf \"%.3f\", $t2 - $t1 }")")
    done
    # median + min + max
    local sorted
    sorted=$(printf '%s\n' "${samples[@]}" | sort -n)
    local count=${#samples[@]}
    local mid=$((count / 2))
    local median
    if ((count % 2 == 1)); then
        median=$(echo "$sorted" | sed -n "$((mid + 1))p")
    else
        local a b
        a=$(echo "$sorted" | sed -n "${mid}p")
        b=$(echo "$sorted" | sed -n "$((mid + 1))p")
        median=$(awk "BEGIN { printf \"%.3f\", ($a + $b) / 2 }")
    fi
    local min_val max_val
    min_val=$(echo "$sorted" | head -1)
    max_val=$(echo "$sorted" | tail -1)
    printf "%-40s median=%ss  min=%ss  max=%ss  (n=%d)\n" "$label" "$median" "$min_val" "$max_val" "$count" | log
}

# -----------------------------------------------------------------------------
# Correctness: bc-seek (--no-ignore --hidden) and find produce the same set.
# -----------------------------------------------------------------------------
correctness_check() {
    echo "--- correctness: bc-seek --no-ignore --hidden =?= find ---" | log
    local bc_out find_out diff_lines
    bc_out="$(mktemp)"
    find_out="$(mktemp)"
    "$BIN" find --type=f --no-ignore --hidden "$TARGET" 2>/dev/null | sort >"$bc_out"
    "$FIND_BIN" "$TARGET" -type f 2>/dev/null | sort >"$find_out"
    diff_lines=$(diff "$bc_out" "$find_out" | wc -l)
    if [[ "$diff_lines" -eq 0 ]]; then
        echo "  OK ($(wc -l <"$bc_out") files matched)" | log
    else
        echo "  MISMATCH: $diff_lines diff lines" | log
        head -5 <(diff "$bc_out" "$find_out") | log
    fi
    rm -f "$bc_out" "$find_out"
    echo "" | log
}

# -----------------------------------------------------------------------------
# Warm benchmarks
# -----------------------------------------------------------------------------
warm_bench() {
    echo "--- warm (cache hot), $WARM_RUNS runs each ---" | log

    # Prime the caches
    "$BIN" find --type=f "$TARGET" >/dev/null 2>&1 || true
    "$FIND_BIN" "$TARGET" -type f >/dev/null 2>&1 || true
    [[ -n "$FD_BIN" ]] && "$FD_BIN" -uu --type=f . "$TARGET" >/dev/null 2>&1 || true

    echo "[scenario: walk --type=f]" | log
    run_series "bc-seek (auto)" "$WARM_RUNS" "$BIN" find --type=f "$TARGET"
    run_series "bc-seek --threads=0" "$WARM_RUNS" "$BIN" --threads=0 find --type=f "$TARGET"
    run_series "bc-seek --no-ignore --hidden" "$WARM_RUNS" "$BIN" find --type=f --no-ignore --hidden "$TARGET"
    run_series "find -type f" "$WARM_RUNS" "$FIND_BIN" "$TARGET" -type f
    [[ -n "$FD_BIN" ]] && run_series "fd -uu --type=f" "$WARM_RUNS" "$FD_BIN" -uu --type=f . "$TARGET"
    echo "" | log

    echo "[scenario: name match *.c]" | log
    run_series "bc-seek (auto) --name='*.c'" "$WARM_RUNS" "$BIN" find --type=f --name='*.c' "$TARGET"
    run_series "find -name '*.c'" "$WARM_RUNS" "$FIND_BIN" "$TARGET" -type f -name "*.c"
    [[ -n "$FD_BIN" ]] && run_series "fd -uu --type=f -e c" "$WARM_RUNS" "$FD_BIN" -uu --type=f -e c . "$TARGET"
    echo "" | log

    echo "[scenario: size > 1 MiB]" | log
    run_series "bc-seek (auto) --size=+1M" "$WARM_RUNS" "$BIN" find --type=f --size=+1M "$TARGET"
    run_series "find -size +1M" "$WARM_RUNS" "$FIND_BIN" "$TARGET" -type f -size +1M
    [[ -n "$FD_BIN" ]] && run_series "fd -uu --type=f --size=+1M" "$WARM_RUNS" "$FD_BIN" -uu --type=f --size=+1M . "$TARGET"
    echo "" | log
}

# -----------------------------------------------------------------------------
# Cold benchmarks (sudo required)
# -----------------------------------------------------------------------------
cold_bench() {
    echo "--- cold (dropped caches), $COLD_RUNS runs each ---" | log

    cold_run() {
        local label="$1"
        shift
        local samples=()
        for ((i = 1; i <= COLD_RUNS; i++)); do
            sync
            echo 3 > /proc/sys/vm/drop_caches
            local t1 t2
            t1=$(date +%s.%N)
            "$@" >/dev/null 2>&1 || true
            t2=$(date +%s.%N)
            samples+=("$(awk "BEGIN { printf \"%.3f\", $t2 - $t1 }")")
        done
        printf "%-40s runs=[%s]\n" "$label" "${samples[*]}" | log
    }

    cold_run "bc-seek (auto)" "$BIN" find --type=f "$TARGET"
    cold_run "find -type f" "$FIND_BIN" "$TARGET" -type f
    [[ -n "$FD_BIN" ]] && cold_run "fd -uu --type=f" "$FD_BIN" -uu --type=f . "$TARGET"
    echo "" | log
}

correctness_check
warm_bench
[[ $WITH_COLD -eq 1 ]] && cold_bench

echo "=== done. full log: $OUT ===" | log
