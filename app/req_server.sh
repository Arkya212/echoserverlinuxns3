#!/usr/bin/env bash
set -euo pipefail

# ── ETSI QKD 014 API Client ──
# Config (override via env: HOST, PORT, SAE, CERT)
HOST="${HOST:-qkd-kms}"
PORT="${PORT:-8443}"
SAE="${SAE:-alice}"     # master_SAE_ID or slave_SAE_ID
CERT="${CERT:-certs/ca_actual.pem}"        # path to CA cert (e.g., ca.pem). If empty, uses -k (insecure)
RESOLVE="${RESOLVE:-qkd-kms:8443:10.7.34.240}"  # hostname resolution override

# curl flags
COMMON_FLAGS=(--silent --show-error --fail)
if [[ -n "$CERT" ]]; then
  TLS_FLAGS=(--cacert "$CERT" --resolve "$RESOLVE")
else
  TLS_FLAGS=(-k)  # allow self-signed
fi

base_url() {
  echo "https://${HOST}:${PORT}/api/v1/keys"
}

# GetStatus - GET /api/v1/keys/{master_SAE_ID}/status
get_status() {
  local sae="${1:-$SAE}"
  curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" "$(base_url)/${sae}/status"
  echo
}

# GetKeys - GET /api/v1/keys/{slave_SAE_ID}/enc_keys?number=N&size=S
get_keys() {
  local sae="${1:-$SAE}"
  local number="${2:-1}"
  local size="${3:-256}"
  curl "${COMMON_FLAGS[@]}" "${TLS_FLAGS[@]}" \
    "$(base_url)/${sae}/enc_keys?number=${number}&size=${size}"
  echo
}

# GetKeysWithKeyIds - POST /api/v1/keys/{master_SAE_ID}/dec_keys
# Body: {"key_IDs": [{"key_ID": "ID_1"}, {"key_ID": "ID_2"}]}
get_keys_with_ids() {
  local sae="${1:-$SAE}"
  shift || true
  if [[ $# -lt 1 ]]; then
    echo "Usage: dec <sae> <@json_file|json_string>" >&2
    echo "  Body: {\"key_IDs\":[{\"key_ID\":\"...\"}]}" >&2
    exit 2
  fi
  local body="$1"
  # Remove --fail flag for dec_keys to see actual response
  curl "${TLS_FLAGS[@]}" \
    -X POST \
    -H "Content-Type: application/json" \
    -d "$body" \
    "$(base_url)/${sae}/dec_keys"
  echo
}

usage() {
  cat <<EOF
ETSI QKD 014 API Client
========================
Usage:
  HOST=qkd-kms PORT=8443 SAE=alice CERT=certs/ca_actual.pem $0 <command> [args]

Commands:
  status [sae]                        - GetStatus()
      GET  /api/v1/keys/{master_SAE_ID}/status

  enc [sae] [number] [size]           - GetKeys()
      GET  /api/v1/keys/{slave_SAE_ID}/enc_keys?number=N&size=S

  dec <sae> <@file|json>              - GetKeysWithKeyIds()
      POST /api/v1/keys/{master_SAE_ID}/dec_keys

Examples:
  $0 status                           # alice status (default SAE)
  $0 status bob                       # bob status
  $0 enc alice 3 256                  # 3 keys, 256 bits each
  $0 enc bob 1                        # 1 key, default size
  $0 dec bob '{"key_IDs":[{"key_ID":"ABC123"}]}'
  CERT=certs/ca.pem $0 status         # with cert verification
EOF
}

cmd="${1:-}"
shift || true
case "$cmd" in
  status)       get_status "$@";;
  enc)          get_keys "$@";;
  dec)          get_keys_with_ids "$@";;
  ""|-h|--help|help) usage;;
  *) echo "Unknown command: $cmd" >&2; usage; exit 1;;
esac
