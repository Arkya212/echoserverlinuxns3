#!/usr/bin/env bash
set -euo pipefail

# QKD Bridge client smoke/load test.
#
# Intended usage:
#   1) Start server: ./ns3 run curl_qkd_bridge
#   2) On the client host: run this script. It will execute a set of HTTPS+mTLS
#      requests (or HTTP fallback) and print ONLY a compact pass/fail summary.
#
# Certificates:
#   By default, the script auto-detects certs in the current directory
#   (./authority.pem and ./self_combined.pem). If they are not present, it
#   falls back to ~/nsim_qkd_certs.

HOST="10.7.34.240"
PORT="8080"
PROTO="https"

CERT_DIR_DEFAULT="$HOME/nsim_qkd_certs"
CERT_DIR=""
CA_CERT=""
CLIENT_CERT=""

# If REQUESTS is 0, the script auto-computes a request count based on the
# current enc_buffer key_count so that it is likely to drain the buffer and
# trigger blocking behavior.
REQUESTS="0"
CONCURRENCY="50"
KEYS_PER_REQUEST="3"

REQUEST_TIMEOUT_SECS="120"
CONNECT_TIMEOUT_SECS="3"
BLOCK_THRESHOLD_SECS="2"
DELAY_MS="0"
ENC_SAE="auto"

VERBOSE="0"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
    --host <ip/hostname>       (default: ${HOST})
    --port <port>              (default: ${PORT})
    --http                     Use plain HTTP (no TLS/mTLS)
    --https                    Use HTTPS+mTLS (default)
    --cert-dir <dir>           (default: auto: ./ or ${CERT_DIR_DEFAULT})
    --requests <N>             Total enc_keys load requests (default: auto)
    --concurrency <N>          Parallel enc_keys requests (default: ${CONCURRENCY})
    --keys-per-request <N>     enc_keys/number/N (default: ${KEYS_PER_REQUEST})
    --timeout <secs>           Per-request max time (default: ${REQUEST_TIMEOUT_SECS})
    --block-threshold <secs>   Counts as "blocked" if latency >= this (default: ${BLOCK_THRESHOLD_SECS})
    --delay-ms <ms>            Sleep this long between spawning requests (default: ${DELAY_MS})
    --enc-sae <alice|bob|auto> Which SAE to use for enc_keys load (default: ${ENC_SAE})
    --verbose                  Prints result directory and a few failed examples

Examples:
    $(basename "$0")
    $(basename "$0") --requests 50 --concurrency 10
    $(basename "$0") --http
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --http) PROTO="http"; shift 1 ;;
        --https) PROTO="https"; shift 1 ;;
        --cert-dir) CERT_DIR="$2"; shift 2 ;;
        --requests) REQUESTS="$2"; shift 2 ;;
        --concurrency) CONCURRENCY="$2"; shift 2 ;;
        --keys-per-request) KEYS_PER_REQUEST="$2"; shift 2 ;;
        --timeout) REQUEST_TIMEOUT_SECS="$2"; shift 2 ;;
        --block-threshold) BLOCK_THRESHOLD_SECS="$2"; shift 2 ;;
        --delay-ms) DELAY_MS="$2"; shift 2 ;;
        --enc-sae) ENC_SAE="$2"; shift 2 ;;
        --verbose) VERBOSE="1"; shift 1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$CERT_DIR" ]]; then
    if [[ -f "./authority.pem" && -f "./self_combined.pem" ]]; then
        CERT_DIR="."
    else
        CERT_DIR="$CERT_DIR_DEFAULT"
    fi
fi

CA_CERT="${CERT_DIR}/authority.pem"
CLIENT_CERT="${CERT_DIR}/self_combined.pem"
BASE_URL="${PROTO}://${HOST}:${PORT}"

need_file() {
    local f="$1"
    if [[ ! -f "$f" ]]; then
        echo "Missing file: $f" >&2
        exit 2
    fi
}

CURL_BASE=(
    curl
    -sS
    --http1.1
    --connect-timeout "${CONNECT_TIMEOUT_SECS}"
    --max-time "${REQUEST_TIMEOUT_SECS}"
)

if [[ "$PROTO" == "https" ]]; then
    need_file "$CA_CERT"
    need_file "$CLIENT_CERT"
    CURL_BASE+=(--cacert "$CA_CERT" --cert "$CLIENT_CERT")
fi

RUN_DIR="$(mktemp -d -t qkd_bridge_test_XXXXXX)"

cleanup() {
    if [[ "$VERBOSE" == "0" ]]; then
        rm -rf "$RUN_DIR"
    fi
}
trap cleanup EXIT

request_once() {
    local name="$1"
    local method="$2"
    local url="$3"
    local payload="${4:-}"

    local body_file="${RUN_DIR}/${name}.body"
    local err_file="${RUN_DIR}/${name}.err"

    local t0_ns
    local t1_ns
    t0_ns=$(date +%s%N)

    local rc=0
    local http_code=""

    if [[ "$method" == "GET" ]]; then
        http_code=$("${CURL_BASE[@]}" -o "$body_file" -w "%{http_code}" "$url" 2>"$err_file") || rc=$?
    else
        http_code=$("${CURL_BASE[@]}" -o "$body_file" -w "%{http_code}" \
            -X "$method" -H "Content-Type: application/json" -d "$payload" "$url" 2>"$err_file") || rc=$?
    fi

    t1_ns=$(date +%s%N)
    local dur_ms=$(( (t1_ns - t0_ns) / 1000000 ))

    local blocked="0"
    if (( dur_ms >= BLOCK_THRESHOLD_SECS * 1000 )); then
        blocked="1"
    fi

    # Treat as pass only if:
    #   - curl succeeded
    #   - HTTP 200
    #   - body does not contain a JSON "error" field
    local pass="0"
    if [[ "$rc" -eq 0 && "$http_code" == "200" ]]; then
        if ! grep -q '"error"' "$body_file"; then
            pass="1"
        fi
    fi

    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$pass" "$rc" "$http_code" "$blocked" >> "${RUN_DIR}/results.tsv"
}

sleep_ms() {
    local ms="$1"
    if [[ -z "$ms" ]]; then
        return
    fi
    if (( ms <= 0 )); then
        return
    fi
    sleep "$(awk -v ms="$ms" 'BEGIN{printf "%.3f", ms/1000}')"
}

extract_buffer_key_count() {
    local body_file="$1"
    local buf="$2"

    awk -v buf="$buf" '
        $0 ~ "\"" buf "_buffer\"" {
            if (index($0, "{") == 0) {
                exit
            }
            in_buf = 1
        }
        in_buf && /\"key_count\"/ {
            gsub(/[^0-9]/, "", $0);
            if (length($0) > 0) {
                print $0;
                exit;
            }
        }
        in_buf && /}/ { exit }
    ' "$body_file" 2>/dev/null || true
}

# 1) Quick connectivity/status checks
request_once "alice_status" "GET" "${BASE_URL}/api/v1/keys/alice/status"
request_once "bob_status" "GET" "${BASE_URL}/api/v1/keys/bob/status"

alice_status_body="${RUN_DIR}/alice_status.body"
bob_status_body="${RUN_DIR}/bob_status.body"

alice_enc_count="$(extract_buffer_key_count "$alice_status_body" "enc")"
bob_enc_count="$(extract_buffer_key_count "$bob_status_body" "enc")"

alice_enc_count="${alice_enc_count:-0}"
bob_enc_count="${bob_enc_count:-0}"

if [[ "$ENC_SAE" == "auto" ]]; then
    if (( bob_enc_count >= alice_enc_count )); then
        ENC_SAE="bob"
        selected_enc_count="$bob_enc_count"
    else
        ENC_SAE="alice"
        selected_enc_count="$alice_enc_count"
    fi
else
    if [[ "$ENC_SAE" == "bob" ]]; then
        selected_enc_count="$bob_enc_count"
    else
        selected_enc_count="$alice_enc_count"
    fi
fi

# 2) One enc_keys sanity request
request_once "enc_default" "GET" "${BASE_URL}/api/v1/keys/${ENC_SAE}/enc_keys"

# 3) Auto-compute load request count if not specified
if [[ "$REQUESTS" == "0" ]]; then
    if (( selected_enc_count > 0 )); then
        req_min=$(( (selected_enc_count / KEYS_PER_REQUEST) + 20 ))
        if (( req_min < 20 )); then
            req_min=20
        fi
        REQUESTS="$req_min"
    else
        REQUESTS="20"
    fi
fi

# 4) Concurrent enc_keys load
enc_url="${BASE_URL}/api/v1/keys/${ENC_SAE}/enc_keys/number/${KEYS_PER_REQUEST}"

active=0
idx=1
completed=0
while (( idx <= REQUESTS )); do
    sleep_ms "$DELAY_MS"
    request_once "enc_${idx}" "GET" "$enc_url" &
    active=$((active + 1))

    if (( active >= CONCURRENCY )); then
        wait -n || true
        active=$((active - 1))
        completed=$((completed + 1))
        if (( completed % 10 == 0 )); then
            echo "progress: ${completed}/${REQUESTS}" >&2
        fi
    fi

    idx=$((idx + 1))
done

while (( active > 0 )); do
    wait -n || true
    active=$((active - 1))
    completed=$((completed + 1))
    if (( completed % 10 == 0 )); then
        echo "progress: ${completed}/${REQUESTS}" >&2
    fi
done

# 5) Summary: print ONLY pass/fail (+ a couple of useful counters)
pass=0
fail=0
blocked=0
timeouts=0

while IFS=$'\t' read -r name did_pass rc http_code did_block; do
    if [[ "$did_pass" == "1" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        if [[ "$rc" == "28" ]]; then
            timeouts=$((timeouts + 1))
        fi
    fi

    if [[ "$did_block" == "1" ]]; then
        blocked=$((blocked + 1))
    fi
done < "${RUN_DIR}/results.tsv"

total=$((pass + fail))

printf 'TOTAL=%s PASSED=%s FAILED=%s' "$total" "$pass" "$fail"
printf ' TIMEOUTS=%s BLOCKED=%s\n' "$timeouts" "$blocked"

if [[ "$VERBOSE" == "1" ]]; then
    echo "Results dir: ${RUN_DIR}"
    echo "Target: ${BASE_URL}"
    echo "requests=${REQUESTS} concurrency=${CONCURRENCY} keys-per-request=${KEYS_PER_REQUEST} delay-ms=${DELAY_MS} enc-sae=${ENC_SAE}"
    echo "Showing up to 5 failures:" 
    awk -F '\t' '$2 == 0 { print $1 " rc=" $3 " http=" $4 " blocked=" $5 }' "${RUN_DIR}/results.tsv" | head -n 5
fi

# Exit non-zero if anything failed.
if (( fail > 0 )); then
    exit 1
fi
