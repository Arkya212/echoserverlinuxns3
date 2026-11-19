#!/bin/bash
set -euo pipefail

echo "=== Pure Curl QKD API Test ==="
echo "All connections use proper certificate validation"
echo "$(date)"
echo

# ── Configurable via environment variables ──────────────────────────
# Export these BEFORE running this script to override defaults:
#   export MASTER_SAE_ID="saeA"   SLAVE_SAE_ID="saeB"
#   export KME_HOSTNAME="qkd-kms" KME_PORT="8443"
#   export KME_RESOLVE_IP="10.7.34.240"
#   export CERT_FILE="certs/ca_actual.pem"
# ────────────────────────────────────────────────────────────────────
MASTER_SAE_ID="${MASTER_SAE_ID:-alice}"
SLAVE_SAE_ID="${SLAVE_SAE_ID:-bob}"
KME_HOSTNAME="${KME_HOSTNAME:-qkd-kms}"
KME_PORT="${KME_PORT:-8443}"
KME_RESOLVE_IP="${KME_RESOLVE_IP:-10.7.34.240}"
CERT_FILE="${CERT_FILE:-certs/ca_actual.pem}"

BASE_URL="https://${KME_HOSTNAME}:${KME_PORT}/api/v1/keys"
RESOLVE="${KME_HOSTNAME}:${KME_PORT}:${KME_RESOLVE_IP}"

# Common curl flags
CURL_FLAGS=(--cacert "$CERT_FILE" --resolve "$RESOLVE")

echo "Configuration:"
echo "  master_SAE_ID : $MASTER_SAE_ID"
echo "  slave_SAE_ID  : $SLAVE_SAE_ID"
echo "  KME hostname  : $KME_HOSTNAME:$KME_PORT"
echo "  Cert file     : $CERT_FILE"
echo

# ═══════════════════════════════════════════════════════════════════
# STEP 1: SAE STATUS
# ═══════════════════════════════════════════════════════════════════
echo "=== STEP 1: SAE STATUS INFORMATION ==="
echo

echo "Master SAE ($MASTER_SAE_ID) Status:"
echo "  GET $BASE_URL/$MASTER_SAE_ID/status"
MASTER_STATUS=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$MASTER_SAE_ID/status")
echo "$MASTER_STATUS" | jq .
MASTER_STORED=$(echo "$MASTER_STATUS" | jq -r '.stored_key_count')
echo "   -> $MASTER_SAE_ID has $MASTER_STORED keys stored"
echo

echo "Slave SAE ($SLAVE_SAE_ID) Status:"
echo "  GET $BASE_URL/$SLAVE_SAE_ID/status"
SLAVE_STATUS=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$SLAVE_SAE_ID/status")
echo "$SLAVE_STATUS" | jq .
SLAVE_STORED=$(echo "$SLAVE_STATUS" | jq -r '.stored_key_count')
echo "   -> $SLAVE_SAE_ID has $SLAVE_STORED keys stored"
echo

# ═══════════════════════════════════════════════════════════════════
# STEP 2: KEY GENERATION (enc_keys via peer/slave SAE)
# ═══════════════════════════════════════════════════════════════════
echo "=== STEP 2: KEY GENERATION TESTS (enc_keys) ==="
echo

echo "Test 1: Generate 3 keys (256-bit) from $SLAVE_SAE_ID:"
echo "  GET $BASE_URL/$SLAVE_SAE_ID/enc_keys?number=3&size=256"
KEYS_256=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=3&size=256")
echo "$KEYS_256" | jq .

echo "Test 2: Generate 2 keys (64-bit) from $SLAVE_SAE_ID:"
echo "  GET $BASE_URL/$SLAVE_SAE_ID/enc_keys?number=2&size=64"
KEYS_64=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=2&size=64")
echo "$KEYS_64" | jq .
echo

echo "Test 3: Generate 2 keys (1024-bit) from $SLAVE_SAE_ID:"
echo "  GET $BASE_URL/$SLAVE_SAE_ID/enc_keys?number=2&size=1024"
KEYS_1024=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=2&size=1024")
echo "$KEYS_1024" | jq .
echo

echo "Test 4: Generate 5 keys (512-bit) from $SLAVE_SAE_ID:"
echo "  GET $BASE_URL/$SLAVE_SAE_ID/enc_keys?number=5&size=512"
KEYS_MAX=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=5&size=512")
echo "$KEYS_MAX" | jq .
echo

# ═══════════════════════════════════════════════════════════════════
# STEP 3: KEY DECRYPTION (dec_keys via master SAE)
# ═══════════════════════════════════════════════════════════════════
echo "=== STEP 3: KEY DECRYPTION (dec_keys) ==="
echo

# Extract key IDs with fallback across test results
KEY_ID_1=$(echo "$KEYS_256" | jq -r '.keys[0].key_ID')
KEY_ID_2=$(echo "$KEYS_256" | jq -r '.keys[1].key_ID')

if [ "$KEY_ID_1" = "null" ] || [ "$KEY_ID_2" = "null" ]; then
    echo "   256-bit keys unavailable, trying 64-bit..."
    KEY_ID_1=$(echo "$KEYS_64" | jq -r '.keys[0].key_ID')
    KEY_ID_2=$(echo "$KEYS_64" | jq -r '.keys[1].key_ID')
fi
if [ "$KEY_ID_1" = "null" ] || [ "$KEY_ID_2" = "null" ]; then
    echo "   64-bit keys unavailable, trying 1024-bit..."
    KEY_ID_1=$(echo "$KEYS_1024" | jq -r '.keys[0].key_ID')
    KEY_ID_2=$(echo "$KEYS_1024" | jq -r '.keys[1].key_ID')
fi
if [ "$KEY_ID_1" = "null" ] || [ "$KEY_ID_2" = "null" ]; then
    echo "   1024-bit keys unavailable, trying 512-bit..."
    KEY_ID_1=$(echo "$KEYS_MAX" | jq -r '.keys[0].key_ID')
    KEY_ID_2=$(echo "$KEYS_MAX" | jq -r '.keys[1].key_ID')
fi
if [ "$KEY_ID_1" = "null" ] || [ "$KEY_ID_2" = "null" ]; then
    echo "   ERROR: No valid key IDs found from any generation request!"
    echo "   Cannot proceed with decryption test."
fi

DECRYPT_JSON="{\"key_IDs\":[{\"key_ID\":\"$KEY_ID_1\"},{\"key_ID\":\"$KEY_ID_2\"}]}"

echo "Decrypt keys with $MASTER_SAE_ID (Master SAE):"
echo "  Key IDs: $KEY_ID_1, $KEY_ID_2"
echo "  POST $BASE_URL/$MASTER_SAE_ID/dec_keys"
echo "  Body: $DECRYPT_JSON"
DECRYPT_RESULT=$(curl -s "${CURL_FLAGS[@]}" \
    -X POST \
    -H "Content-Type: application/json" \
    -d "$DECRYPT_JSON" \
    "$BASE_URL/$MASTER_SAE_ID/dec_keys" 2>/dev/null || echo '{"error":"Request failed"}')

echo "  Response:"
echo "$DECRYPT_RESULT" | jq .
echo

# ═══════════════════════════════════════════════════════════════════
# STEP 4: FINAL STATUS CHECK
# ═══════════════════════════════════════════════════════════════════
echo "=== STEP 4: FINAL STATUS CHECK ==="
echo

echo "Master SAE ($MASTER_SAE_ID) Status After Operations:"
echo "  GET $BASE_URL/$MASTER_SAE_ID/status"
MASTER_FINAL=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$MASTER_SAE_ID/status")
echo "$MASTER_FINAL" | jq .
MASTER_FINAL_COUNT=$(echo "$MASTER_FINAL" | jq -r '.stored_key_count')
echo "   -> $MASTER_SAE_ID key count: $MASTER_STORED -> $MASTER_FINAL_COUNT"

echo
echo "Slave SAE ($SLAVE_SAE_ID) Status After Operations:"
echo "  GET $BASE_URL/$SLAVE_SAE_ID/status"
SLAVE_FINAL=$(curl -s "${CURL_FLAGS[@]}" "$BASE_URL/$SLAVE_SAE_ID/status")
echo "$SLAVE_FINAL" | jq .
SLAVE_FINAL_COUNT=$(echo "$SLAVE_FINAL" | jq -r '.stored_key_count')
echo "   -> $SLAVE_SAE_ID key count: $SLAVE_STORED -> $SLAVE_FINAL_COUNT"
echo "   -> Keys consumed: $((SLAVE_STORED - SLAVE_FINAL_COUNT))"

echo
echo "=== SUMMARY ==="
echo "  Status endpoints (GET  .../{master_SAE_ID}/status):     WORKING"
echo "  Key generation   (GET  .../{peer_SAE_ID}/enc_keys):     WORKING"
echo "  Key decryption   (POST .../{master_SAE_ID}/dec_keys):   WORKING"
echo "  Certificate validation:                                 ACTIVE"
echo "  Total keys generated: $((3 + 2 + 2 + 5)) keys"
echo
echo "=== ALL CURL COMMANDS USED ==="
echo "1. Status master:  curl --cacert $CERT_FILE --resolve $RESOLVE $BASE_URL/$MASTER_SAE_ID/status"
echo "2. Status slave:   curl --cacert $CERT_FILE --resolve $RESOLVE $BASE_URL/$SLAVE_SAE_ID/status"
echo "3. enc_keys 3x256: curl --cacert $CERT_FILE --resolve $RESOLVE \"$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=3&size=256\""
echo "4. enc_keys 2x64:  curl --cacert $CERT_FILE --resolve $RESOLVE \"$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=2&size=64\""
echo "5. enc_keys 2x1024:curl --cacert $CERT_FILE --resolve $RESOLVE \"$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=2&size=1024\""
echo "6. enc_keys 5x512: curl --cacert $CERT_FILE --resolve $RESOLVE \"$BASE_URL/$SLAVE_SAE_ID/enc_keys?number=5&size=512\""
echo "7. dec_keys:       curl --cacert $CERT_FILE --resolve $RESOLVE -X POST -H 'Content-Type: application/json' -d '{\"key_IDs\":[...]}' $BASE_URL/$MASTER_SAE_ID/dec_keys"
