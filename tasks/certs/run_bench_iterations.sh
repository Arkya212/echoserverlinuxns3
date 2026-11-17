#!/usr/bin/env bash
set -euo pipefail

# Run the QKD bridge benchmark (Alice -> Bob, 4 req/s slice) several times and
# print ONE table with per-iteration results, the pass count and the averages.
#
# Usage: ./run_bench_iterations.sh [iterations] [cooldown_secs]
#
# Between two runs the script waits for the cooldown and then waits (bounded)
# until alice enc and bob dec both hold at least MIN_READY_KEYS ready keys, so
# every run starts from a refilled buffer.

ITERATIONS="${1:-8}"
COOLDOWN_SECS="${2:-15}"
HOST="127.0.0.1"
PORT="8080"
MIN_READY_KEYS="125"
BUFFER_WAIT_TIMEOUT_SECS="120"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_SCRIPT="${SCRIPT_DIR}/bench_qkd_bridge.sh"
RESULTS_DIR="/tmp/qkd_bench_iterations_$$"
mkdir -p "$RESULTS_DIR"

say() {
    echo "$@" >&2
}

# Print "<alice enc ready keys> <bob dec ready keys>" or "0 0" when unreachable.
buffer_levels() {
    local alice bob
    alice=$(curl -s --connect-timeout 2 --max-time 5 \
        "http://${HOST}:${PORT}/api/v1/keys/alice/status" 2>/dev/null \
        | grep -o '"key_count": *[0-9]*' | head -1 | grep -o '[0-9]*$' || true)
    bob=$(curl -s --connect-timeout 2 --max-time 5 \
        "http://${HOST}:${PORT}/api/v1/keys/bob/status" 2>/dev/null \
        | grep -o '"key_count": *[0-9]*' | sed -n '2p' | grep -o '[0-9]*$' || true)
    echo "${alice:-0} ${bob:-0}"
}

wait_for_buffers() {
    local waited=0 alice bob
    while (( waited <= BUFFER_WAIT_TIMEOUT_SECS )); do
        read -r alice bob <<< "$(buffer_levels)"
        if (( alice >= MIN_READY_KEYS && bob >= MIN_READY_KEYS )); then
            say "  buffers ready: alice enc=${alice} bob dec=${bob}"
            return 0
        fi
        if (( waited % 10 == 0 )); then
            say "  waiting for buffers (alice enc=${alice} bob dec=${bob}, need >= ${MIN_READY_KEYS}) ..."
        fi
        sleep 2
        waited=$(( waited + 2 ))
    done
    say "  WARNING: buffers still low after ${BUFFER_WAIT_TIMEOUT_SECS}s, running anyway"
    return 0
}

say "=== QKD Bridge Iteration Runner (Alice -> Bob) ==="
say "Iterations: ${ITERATIONS}, cooldown: ${COOLDOWN_SECS}s, min ready keys: ${MIN_READY_KEYS}"
say "Results dir: ${RESULTS_DIR}"
say ""

for (( i = 1; i <= ITERATIONS; i++ )); do
    if (( i > 1 )); then
        say "Cooling down ${COOLDOWN_SECS}s ..."
        sleep "$COOLDOWN_SECS"
    fi
    say "Iteration ${i}/${ITERATIONS}"
    wait_for_buffers
    if "$BENCH_SCRIPT" > "${RESULTS_DIR}/iter_${i}.txt" 2>&1; then
        say "  Result: PASS"
    else
        say "  Result: FAIL"
    fi
done

# One row per iteration: iter verdict enc_total enc_ok enc_avg enc_p95 dec_total dec_ok dec_avg dec_p95
ROWS="${RESULTS_DIR}/rows.tsv"
: > "$ROWS"
for (( i = 1; i <= ITERATIONS; i++ )); do
    out_file="${RESULTS_DIR}/iter_${i}.txt"
    verdict="FAIL"
    if grep -q "VERDICT: PASS" "$out_file"; then
        verdict="PASS"
    fi
    enc_line=$(grep "^enc_alice_rate4" "$out_file" || true)
    dec_line=$(grep "^dec_bob_rate4" "$out_file" || true)
    printf '%s\t%s\t%s\t%s\n' "$i" "$verdict" "$enc_line" "$dec_line" >> "$ROWS"
done

say ""
awk -F'\t' '
    function field(line, n,    parts) {
        split(line, parts, " ")
        return (parts[n] == "") ? 0 : parts[n]
    }
    BEGIN {
        printf "%-6s %-7s | %6s %6s %8s %8s | %6s %6s %8s %8s\n", \
            "ITER", "VERDICT", "ENC_T", "ENC_OK", "ENC_AVG", "ENC_P95", \
            "DEC_T", "DEC_OK", "DEC_AVG", "DEC_P95"
        print "---------------------------------------------------------------------------------"
    }
    {
        et = field($3, 2); eo = field($3, 3); ea = field($3, 7); ep = field($3, 8)
        dt = field($4, 2); do_ = field($4, 3); da = field($4, 7); dp = field($4, 8)
        printf "%-6s %-7s | %6d %6d %8d %8d | %6d %6d %8d %8d\n", \
            $1, $2, et, eo, ea, ep, dt, do_, da, dp
        n++
        if ($2 == "PASS") passed++
        set[0] += et; set[1] += eo; set[2] += ea; set[3] += ep
        set[4] += dt; set[5] += do_; set[6] += da; set[7] += dp
    }
    END {
        print "---------------------------------------------------------------------------------"
        if (n == 0) { exit }
        printf "%-6s %-7s | %6.1f %6.1f %8.1f %8.1f | %6.1f %6.1f %8.1f %8.1f\n", \
            "AVG", "", set[0]/n, set[1]/n, set[2]/n, set[3]/n, set[4]/n, set[5]/n, set[6]/n, set[7]/n
        printf "\nPASSED 4 req/s requirement: %d / %d iterations\n", passed, n
    }
' "$ROWS"

say ""
say "Per-run outputs kept in: ${RESULTS_DIR}"

if awk -F'\t' '$2 != "PASS" { failed = 1 } END { exit failed }' "$ROWS"; then
    exit 0
fi
exit 1
