#!/usr/bin/env bash
set -euo pipefail

# QKD Bridge paired-flow benchmark harness.
#
# Purpose:
#   Measure paired ETSI 014 flows without an unbounded stress phase:
#
#       encryptor/enc_keys -> peer/dec_keys?key_ID=<matching-id>
#
#   Exactly ONE key is requested per enc_keys request. Each pair consumes one
#   key, so the default run is deliberately bounded to 1, 2, and 4 pairs/s.
#
# APIs exercised:
#   GET  /api/v1/keys/{alice,bob}/status
#   GET  /api/v1/keys/{alice,bob}/enc_keys            (1 key)
#   GET  /api/v1/keys/{alice,bob}/enc_keys?number=1&size=256  (rcpd form)
#   GET  /api/v1/keys/{bob,alice}/dec_keys?key_ID=<id>        (rcpd form)
#   POST /api/v1/keys/{bob,alice}/dec_keys                    (JSON body form)
#
# Certificates:
#   HTTPS mode needs authority.pem + self_combined.pem. Auto-detect order:
#     1. <cert-dir> given via --cert-dir
#     2. current directory
#     3. contrib/echoserverlinuxns3/tasks/certs (when run from the ns-3 root)
#     4. $HOME/nsim_qkd_certs
#
# Usage:
#   Start the server on this machine, then:
#     ./bench_qkd_bridge.sh                 # HTTPS against 127.0.0.1:8080
#     ./bench_qkd_bridge.sh --http          # plain HTTP comparison
#     ./bench_qkd_bridge.sh --host 10.7.34.240
#     ./bench_qkd_bridge.sh --quick         # short smoke version
#
# Output: directional summary tables and an overall PASS/FAIL verdict.

# Fixed local benchmark configuration. Run this script without arguments for
# the standard Alice->Bob sweep.
HOST="127.0.0.1"
PORT="8080"
PROTO="https"
DURATION="10"
# Per-request hard timeout. Must exceed REQUEST_TIMEOUT_SEC on the server (5s)
# so a parked request can observe its 503/504 response instead of curl dying.
REQ_TIMEOUT_SECS="10"
CONN_TIMEOUT_SECS="3"

# Request-rate sweep (requests per second) for enc/dec key endpoints.
RATES=("1" "2" "4")
# Concurrency levels for the status/burst tests.
BURSTS=("1")
# Final connection-limit stress burst size.
STRESS_BURST="0"
ENC_SAE="alice"
DEC_SAE="bob"

CERT_DIR=""
KEEP="0"
VERBOSE="0"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
    --host <ip/hostname>   Server host (default: ${HOST}, localhost)
    --port <port>          Server port (default: ${PORT})
    --http                 Use plain HTTP (no TLS/mTLS)
    --https                Use HTTPS+mTLS (default)
    --cert-dir <dir>       Directory with authority.pem + self_combined.pem
    --duration <secs>      Duration of each rate-test slice (default: ${DURATION})
    --rates <r1,r2,...>    Request-rate sweep for enc/dec (default: ${RATES[*]})
    --burst <n1,n2,...>    Burst sizes for status (default: ${BURSTS[*]})
    --stress <N>           Parallel connections for stress test (default: ${STRESS_BURST})
    --reverse               Test Bob->Alice instead of Alice->Bob
    --quick                Shortcut: duration 3, rates 1,2,4, burst 1
    --keep                 Keep the results directory
    --verbose              Print extra detail
    -h, --help             Show this help

Exit code: 0 if the benchmark meets the minimum requirement, 1 otherwise.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --http) PROTO="http"; shift 1 ;;
        --https) PROTO="https"; shift 1 ;;
        --cert-dir) CERT_DIR="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --rates) IFS=',' read -r -a RATES <<< "$2"; shift 2 ;;
        --burst) IFS=',' read -r -a BURSTS <<< "$2"; shift 2 ;;
        --stress) STRESS_BURST="$2"; shift 2 ;;
        --reverse) ENC_SAE="bob"; DEC_SAE="alice"; shift 1 ;;
        --quick) DURATION="3"; RATES=("1" "2" "4"); BURSTS=("1"); STRESS_BURST="0"; shift 1 ;;
        --keep) KEEP="1"; shift 1 ;;
        --verbose) VERBOSE="1"; KEEP="1"; shift 1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
    esac
done

# ---------------------------------------------------------------------------
# Certificate handling
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

detect_cert_dir() {
    local candidates=(
        "$(pwd)"
        "${SCRIPT_DIR}"
        "${HOME}/nsim_qkd_certs"
    )
    local d
    for d in "${candidates[@]}"; do
        if [[ -f "$d/authority.pem" && -f "$d/self_combined.pem" ]]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

if [[ -z "$CERT_DIR" ]]; then
    if CERT_DIR="$(detect_cert_dir)"; then
        :
    else
        echo "ERROR: authority.pem / self_combined.pem not found." >&2
        echo "Looked in: pwd, ${SCRIPT_DIR}, \$HOME/nsim_qkd_certs" >&2
        echo "Copy them from contrib/echoserverlinuxns3/tasks/certs or pass --cert-dir." >&2
        exit 2
    fi
fi

CA_CERT="${CERT_DIR}/authority.pem"
CLIENT_CERT="${CERT_DIR}/self_combined.pem"
BASE_URL="${PROTO}://${HOST}:${PORT}"

CURL_BASE=(
    curl
    -sS
    --http1.1
    --connect-timeout "${CONN_TIMEOUT_SECS}"
    --max-time "${REQ_TIMEOUT_SECS}"
)
if [[ "$PROTO" == "https" ]]; then
    CURL_BASE+=(--cacert "$CA_CERT" --cert "$CLIENT_CERT")
fi

RUN_DIR="$(mktemp -d -t qkd_bridge_bench_XXXXXX)"
RESULTS="${RUN_DIR}/results.tsv"
SUMMARY="${RUN_DIR}/summary.tsv"
: > "$RESULTS"
: > "$SUMMARY"

cleanup() {
    if [[ "$KEEP" == "0" ]]; then
        rm -rf "$RUN_DIR"
    fi
}
trap cleanup EXIT

say() {
    echo "$@" >&2
}

# ---------------------------------------------------------------------------
# Single request runner. Appends one line to $RESULTS:
#   test <TAB> idx <TAB> op <TAB> http_code <TAB> latency_ms <TAB> rc <TAB> ok
# ok=1  <=>  curl rc 0 AND HTTP 200 AND body has no "error"
# ---------------------------------------------------------------------------
fire_req() {
    local test="$1"
    local idx="$2"
    local op="$3"       # logical endpoint label
    local url="$4"
    local method="${5:-GET}"
    local payload="${6:-}"

    local body_file="${RUN_DIR}/body_${test}_${idx}.json"
    local t0_ns t1_ns rc http_code latency_ms ok

    t0_ns=$(date +%s%N)
    rc=0
    if [[ "$method" == "GET" ]]; then
        http_code=$("${CURL_BASE[@]}" -o "$body_file" -w "%{http_code}" "$url" 2>/dev/null) || rc=$?
    else
        http_code=$("${CURL_BASE[@]}" -o "$body_file" -w "%{http_code}" \
            -X "$method" -H "Content-Type: application/json" -d "$payload" "$url" 2>/dev/null) || rc=$?
    fi
    t1_ns=$(date +%s%N)

    latency_ms=$(( (t1_ns - t0_ns) / 1000000 ))
    [[ -n "$http_code" ]] || http_code="000"

    ok="0"
    if [[ "$rc" -eq 0 && "$http_code" == "200" ]]; then
        if ! grep -q '"error"' "$body_file" 2>/dev/null; then
            ok="1"
        fi
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$test" "$idx" "$op" "$http_code" "$latency_ms" "$rc" "$ok" >> "$RESULTS"
}

# ---------------------------------------------------------------------------
# Rate test: fire <rate> requests/second for $DURATION seconds.
# ---------------------------------------------------------------------------
run_rate_test() {
    local test="$1"   # label
    local op="$2"     # endpoint label
    local url="$3"
    local rate="$4"
    local ids_file="${5:-}"  # optional: file with key_IDs to iterate (dec tests)
    local method="${6:-GET}"
    local payload_tmpl="${7:-}"
    local ids_offset="${8:-1}" # 1-based start line in ids_file (dec slices must not reuse IDs)
    local total_override="${9:-}" # optional cap on requests (e.g., fewer IDs warmed than planned)

    local total=$(( DURATION * rate ))
    if [[ -n "$total_override" ]] && (( total_override < total )); then
        total=$total_override
    fi
    if (( total <= 0 )); then
        say ">>> ${test}: SKIPPED (no requests to fire)"
        SUMMARY_WALL[$test]="0.01"
        return
    fi
    local delay_ms
    delay_ms=$(awk "BEGIN{printf \"%d\", 1000/${rate}}")

    local i=0
    local id=""
    local t_start t_end

    say ">>> ${test}: ${rate} req/s for ${DURATION}s (${total} requests): ${url}"
    t_start=$(date +%s%N)

    while (( i < total )); do
        id=""
        if [[ -n "$ids_file" ]]; then
            id=$(sed -n "$(( ids_offset + i ))p" "$ids_file" || true)
        fi

        local actual_url="$url"
        local actual_payload="$payload_tmpl"
        [[ -n "$id" ]] && actual_url="${url//KEYID/$id}"
        [[ -n "$id" && -n "$payload_tmpl" ]] && actual_payload="${payload_tmpl//KEYID/$id}"

        fire_req "$test" "$i" "$op" "$actual_url" "$method" "$actual_payload" &
        i=$(( i + 1 ))

        # fractional second sleep: use nanosecond arithmetic via sleep X.Y
        if (( delay_ms > 0 )); then
            sleep "$(awk "BEGIN{printf \"%.4f\", ${delay_ms}/1000}")"
        fi
    done
    wait
    t_end=$(date +%s%N)

    local wall_s
    wall_s=$(awk "BEGIN{printf \"%.2f\", ($t_end - $t_start)/1000000000}")
    SUMMARY_WALL[$test]="$wall_s"
}

# ---------------------------------------------------------------------------
# Burst test: fire <count> requests concurrently.
# ---------------------------------------------------------------------------
run_burst_test() {
    local test="$1"
    local op="$2"
    local url="$3"
    local count="$4"

    say ">>> ${test}: ${count} concurrent: ${url}"
    local t_start t_end i
    t_start=$(date +%s%N)
    for (( i = 0; i < count; i++ )); do
        fire_req "$test" "$i" "$op" "$url" &
    done
    wait
    t_end=$(date +%s%N)

    local wall_s
    wall_s=$(awk "BEGIN{printf \"%.2f\", ($t_end - $t_start)/1000000000}")
    SUMMARY_WALL[$test]="$wall_s"
}

declare -A SUMMARY_WALL=()

# ---------------------------------------------------------------------------
# Aggregate one test slice and persist a summary line.
# ---------------------------------------------------------------------------
summarize() {
    local test="$1"
    local wall="${SUMMARY_WALL[$test]:-1}"

    awk -F'\t' -v t="$test" -v w="$wall" '
        $1 == t {
            n++
            lat = $5
            tot += lat
            if (n == 1 || lat < min) min = lat
            if (lat > max) max = lat
            if ($7 == 1) ok++
            else if ($3 == "000") to++
            else fail++
            a[n] = lat
        }
        END {
            if (n == 0) { printf "%s\t0\t0\t0\t0\t0\t0\t0\t0\t0\n", t; exit }
            # insertion sort for p95 (n is modest here)
            for (i = 2; i <= n; i++) {
                v = a[i]; j = i - 1
                while (j >= 1 && a[j] > v) { a[j+1] = a[j]; j-- }
                a[j+1] = v
            }
            p95i = int(n * 0.95 + 0.5); if (p95i < 1) p95i = 1; if (p95i > n) p95i = n
            printf "%s\t%d\t%d\t%d\t%d\t%d\t%.0f\t%d\t%d\t%.2f\n", \
                t, n, ok, fail, to, min, tot/n, a[p95i], max, ok / w
        }' "$RESULTS" >> "$SUMMARY"
}

# ---------------------------------------------------------------------------
# One paired flow: enc_keys on the encryptor SAE, then dec_keys on the
# decryptor SAE using the key_ID that enc_keys returned. Each call is recorded
# as its own row (enc_test / dec_test). dec is only attempted when enc returned
# a key_ID. The key value returned by dec must equal the one returned by enc,
# otherwise a "keymismatch" row is recorded.
# Both sides consume exactly one key, so neither SBuffer saturates.
# ---------------------------------------------------------------------------
fire_pair() {
    local enc_test="$1"
    local dec_test="$2"
    local idx="$3"
    local dec_method="${4:-GET}"

    local enc_body="${RUN_DIR}/body_${enc_test}_${idx}.json"
    local dec_body="${RUN_DIR}/body_${dec_test}_${idx}.json"
    local kid enc_val dec_val dec_url dec_payload=""

    fire_req "$enc_test" "$idx" "enc" \
        "${BASE_URL}/api/v1/keys/${ENC_SAE}/enc_keys?number=1"

    kid=$(grep -o '"key_ID"[^,}]*' "$enc_body" 2>/dev/null | head -1 | cut -d'"' -f4 || true)
    if [[ -z "$kid" ]]; then
        return 0
    fi
    enc_val=$(grep -o '"key"[^,}]*' "$enc_body" 2>/dev/null | head -1 | cut -d'"' -f4 || true)

    if [[ "$dec_method" == "POST" ]]; then
        dec_url="${BASE_URL}/api/v1/keys/${DEC_SAE}/dec_keys"
        dec_payload="{\"key_IDs\":[{\"key_ID\":\"${kid}\"}]}"
    else
        dec_url="${BASE_URL}/api/v1/keys/${DEC_SAE}/dec_keys?key_ID=${kid}"
    fi
    fire_req "$dec_test" "$idx" "dec" "$dec_url" "$dec_method" "$dec_payload"

    dec_val=$(grep -o '"key"[^,}]*' "$dec_body" 2>/dev/null | head -1 | cut -d'"' -f4 || true)
    if [[ -n "$dec_val" && "$dec_val" != "$enc_val" ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "keymismatch_${ENC_SAE}_to_${DEC_SAE}" "$idx" "dec" "200" "0" "0" "0" >> "$RESULTS"
    fi
}

# ---------------------------------------------------------------------------
# Paired rate test: start <rate> paired flows per second for $DURATION seconds.
# ---------------------------------------------------------------------------
run_pair_rate_test() {
    local rate="$1"
    local enc_test="enc_${ENC_SAE}_rate${rate}"
    local dec_test="dec_${DEC_SAE}_rate${rate}"
    local total=$(( DURATION * rate ))
    local delay_s i=0 t_start t_end wall_s

    delay_s=$(awk "BEGIN{printf \"%.4f\", 1/${rate}}")
    say ">>> ${ENC_SAE}->${DEC_SAE}: ${rate} paired flows/s for ${DURATION}s (${total} flows = ${total} enc + ${total} dec, 1 key each)"
    t_start=$(date +%s%N)
    while (( i < total )); do
        fire_pair "$enc_test" "$dec_test" "$i" &
        i=$(( i + 1 ))
        sleep "$delay_s"
    done
    wait
    t_end=$(date +%s%N)

    wall_s=$(awk "BEGIN{printf \"%.2f\", ($t_end - $t_start)/1000000000}")
    SUMMARY_WALL[$enc_test]="$wall_s"
    SUMMARY_WALL[$dec_test]="$wall_s"
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
say "=== QKD Bridge Benchmark ==="
say "Target:  ${BASE_URL}  (proto=${PROTO})"
say "Direction: ${ENC_SAE} enc_keys -> ${DEC_SAE} dec_keys"
say "Certs:   ${CERT_DIR}"
say "Rates:   ${RATES[*]} req/s, duration=${DURATION}s per slice"
say "Bursts:  ${BURSTS[*]} (status), stress=${STRESS_BURST}"
say "Run dir: ${RUN_DIR}"
say ""

# 1) HTTP connectivity sanity check (bypasses TLS entirely)
if ! curl -sS --connect-timeout 2 --max-time 5 \
        -o /dev/null -w "%{http_code}" \
        "http://${HOST}:${PORT}/api/v1/keys/alice/status" | grep -q "200"; then
    say "ERROR: cannot reach ${BASE_URL} (plain HTTP status check failed)."
    say "Is the bridge running?    ./ns3 run curl_qkd_bridge"
    exit 1
fi
say "Preflight HTTP status: OK"

# 2) Requested protocol sanity check
code=$("${CURL_BASE[@]}" -o /dev/null -w "%{http_code}" \
        "${BASE_URL}/api/v1/keys/alice/status" 2>/dev/null || echo "000")
if [[ "$code" != "200" ]]; then
    say "ERROR: ${PROTO} status check returned http=${code}."
    say "If using HTTPS, verify certificates first (see QKD_BRIDGE_SETUP.md)."
    exit 1
fi
say "Preflight ${PROTO} status: OK"
say ""

# ---------------------------------------------------------------------------
# PHASE 1: status bursts (no keys consumed; isolates connection handling)
# ---------------------------------------------------------------------------
say "=== PHASE 1: status endpoint burst tests ==="
for c in "${BURSTS[@]}"; do
    run_burst_test "status_alice_burst${c}" "status" \
        "${BASE_URL}/api/v1/keys/alice/status" "$c"
    summarize "status_alice_burst${c}"
done
for c in "${BURSTS[@]}"; do
    run_burst_test "status_bob_burst${c}" "status" \
        "${BASE_URL}/api/v1/keys/bob/status" "$c"
    summarize "status_bob_burst${c}"
done

# ---------------------------------------------------------------------------
# PHASE 2: paired flows (enc_keys -> dec_keys with the returned key_ID)
# ---------------------------------------------------------------------------
say ""
say "=== PHASE 2: paired enc/dec flows ${ENC_SAE}->${DEC_SAE} (1 key per call) ==="
say "(every enc key is immediately fetched by ID on the peer, so both SBuffers drain evenly)"
for r in "${RATES[@]}"; do
    run_pair_rate_test "$r"
    summarize "enc_${ENC_SAE}_rate${r}"
    summarize "dec_${DEC_SAE}_rate${r}"
done

# Small POST-body sanity check (JSON form used by curl clients), paired as well
POST_PAIRS=4
say ">>> ${POST_PAIRS} paired flows with POST dec_keys (JSON body form)"
ptime=$(date +%s%N)
for (( i = 0; i < POST_PAIRS; i++ )); do
    fire_pair "enc_${ENC_SAE}_post" "dec_${DEC_SAE}_post" "$i" "POST" &
done
wait
SUMMARY_WALL[enc_${ENC_SAE}_post]=$(awk "BEGIN{printf \"%.2f\", ($(date +%s%N) - $ptime)/1000000000}")
SUMMARY_WALL[dec_${DEC_SAE}_post]="${SUMMARY_WALL[enc_${ENC_SAE}_post]}"
summarize "enc_${ENC_SAE}_post"
summarize "dec_${DEC_SAE}_post"

if grep -q "^keymismatch_" "$RESULTS"; then
    SUMMARY_WALL["keymismatch_${ENC_SAE}_to_${DEC_SAE}"]="1"
    summarize "keymismatch_${ENC_SAE}_to_${DEC_SAE}"
fi

# ---------------------------------------------------------------------------
# PHASE 4: optional stress (disabled by default to avoid wasting keys)
# ---------------------------------------------------------------------------
say ""
if (( STRESS_BURST > 0 )); then
    say "=== PHASE 4: stress - ${STRESS_BURST} simultaneous enc_keys ==="
    run_burst_test "stress_enc_alice_${STRESS_BURST}" "enc" \
            "${BASE_URL}/api/v1/keys/${ENC_SAE}/enc_keys?number=1" "$STRESS_BURST"
    summarize "stress_enc_alice_${STRESS_BURST}"
else
    say "=== PHASE 4: stress disabled (no extra keys consumed) ==="
fi

# ---------------------------------------------------------------------------
# Final report
# ---------------------------------------------------------------------------
say ""
printf '%-26s %6s %6s %6s %6s %8s %8s %8s %8s %9s\n' \
    "TEST" "TOTAL" "OK" "FAIL" "TMOUT" "MINms" "AVGms" "P95ms" "MAXms" "PASS/s"
printf '%s\n' "-------------------------------------------------------------------------------------------------"

overall="PASS"
while IFS=$'\t' read -r test tot ok fail to min avg p95 max rate; do
    [[ "$tot" == "0" ]] && { printf '%-26s %6s SKIP (no data)\n' "$test" "0"; continue; }
    printf '%-26s %6s %6s %6s %6s %8s %8s %8s %8s %9s\n' \
        "$test" "$tot" "$ok" "$fail" "$to" "$min" "$avg" "$p95" "$max" "$rate"
done < "$SUMMARY"

printf '%s\n' "-------------------------------------------------------------------------------------------------"

# Minimum-requirement check: the 4-req/s Alice->Bob enc/dec slices must
# complete successfully. This is the highest default rate and uses one key
# per paired flow.
check_slice() {
    local name="$1"
    awk -F'\t' -v t="$name" '
        $1 == t {
            ok_ratio = ($2 > 0) ? $3 / $2 : 0
            if ($10 < 3.6 || ok_ratio < 0.95) { printf "FAIL"; exit }
            printf "OK"
            exit
        }' "$SUMMARY"
}

enc_check="$(check_slice "enc_${ENC_SAE}_rate4")"
dec_check="$(check_slice "dec_${DEC_SAE}_rate4")"

[[ "$enc_check" == "OK" ]] && [[ "$dec_check" == "OK" ]] || overall="FAIL"
if grep -q "^keymismatch_" "$RESULTS"; then
    say "ERROR: enc/dec key values did not match for at least one key_ID"
    overall="FAIL"
fi

say ""
say "Minimum requirement (4 paired flows per second, 1 key per flow):"
say "  ${ENC_SAE} enc_keys @4 req/s:  ${enc_check:-NO DATA}"
say "  ${DEC_SAE} dec_keys @4 req/s:  ${dec_check:-NO DATA}"
say ""
say "VERDICT: ${overall}"
if [[ "$KEEP" == "1" ]]; then
    say "Results kept in: ${RUN_DIR}"
fi

[[ "$overall" == "PASS" ]]
