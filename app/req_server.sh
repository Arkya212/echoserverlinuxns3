#!/usr/bin/env bash
set -euo pipefail

# Config (override via env: HOST, PORT, SAE, CERT)
HOST="${HOST:-10.7.34.240}"
PORT="${PORT:-8443}"
SAE="${SAE:-alice}"     # alice or bob
CERT="${CERT:-}"        # path to CA cert (e.g., cert.pem). If empty, uses -k (insecure)

# curl flags
COMMON_FLAGS=(--silent --show-error --fail)
if [[ -n "$CERT" ]]; then
  TLS_FLAGS=(--cacert "$CERT")
else
  TLS_FLAGS=(-k)  # allow self-signed
fi

base_url() {
  echo "https://${HOST}:${PORT}/api/v1/keys"
}

status() {
  local sae="${1:-$SAE}"
  curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" "$(base_url)/${sae}/status"
  echo
}

enc_keys() {
  local sae="${1:-$SAE}"
  curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" "$(base_url)/${sae}/enc_keys"
  echo
}

enc_keys_n() {
  local sae="${1:-$SAE}"
  local n="${2:-1}"
  curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" "$(base_url)/${sae}/enc_keys/number/${n}"
  echo
}

# dec_keys expects a JSON body:
# {
#   "key_IDs": [
#     {"key_ID": "ID_1"},
#     {"key_ID": "ID_2"}
#   ]
# }
# Usage options:
#   dec_keys <sae> @path/to/body.json
#   dec_keys <sae> '{"key_IDs":[{"key_ID":"..."}]}'
dec_keys() {
  local sae="${1:-$SAE}"
  shift || true
  if [[ $# -lt 1 ]]; then
    echo "Usage: dec_keys <sae> <@json_file|json_string>" >&2
    exit 2
  fi
  local body="$1"
  if [[ "$body" =~ ^@ ]]; then
    curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" \
      -H "Content-Type: application/json" \
      --data-binary "$body" \
      "$(base_url)/${sae}/dec_keys"
  else
    curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" \
      -H "Content-Type: application/json" \
      --data-binary "$body" \
      "$(base_url)/${sae}/dec_keys"
  fi
  echo
}

usage() {
  cat <<EOF
Usage:
  HOST=localhost PORT=8443 SAE=alice CERT=cert.pem $0 <command> [args]

Commands:
  status [sae]                 - GET  /{sae}/status
  enc [sae]                    - GET  /{sae}/enc_keys
  encn [sae] [N]               - GET  /{sae}/enc_keys/number/{N}
  dec <sae> <@file|json>       - POST /{sae}/dec_keys

Examples:
  $0 status
  SAE=bob $0 status
  $0 enc
  $0 encn alice 3
  $0 dec bob '{"key_IDs":[{"key_ID":"ID_1"},{"key_ID":"ID_2"}]}'
  CERT=cert.pem $0 status           # verify with CA instead of -k
EOF
}

cmd="${1:-}"
shift || true
case "$cmd" in
  status) status "$@";;
  enc)    enc_keys "$@";;
  encn)   enc_keys_n "$@";;
  dec)    dec_keys "$@";;
  ""|-h|--help|help) usage;;
  *) echo "Unknown command: $cmd" >&2; usage; exit 1;;
esac