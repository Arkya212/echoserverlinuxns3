#!/usr/bin/env bash
set -euo pipefail

# Generate CA + server certs locally (no system-wide install needed)
# Usage: ./gen_certs.sh [SERVER_IP]
# Certs are written to the current directory.

SERVER_IP="${1:-10.7.34.240}"
CERT_DIR="$(cd "$(dirname "$0")" && pwd)/certs"
mkdir -p "$CERT_DIR"

echo "==> Generating certs in ${CERT_DIR} for IP=${SERVER_IP} ..."

# 1. CA key + cert
openssl genrsa -out "${CERT_DIR}/ca.key" 2048 2>/dev/null
openssl req -new -x509 -days 365 \
    -key "${CERT_DIR}/ca.key" \
    -out "${CERT_DIR}/ca.pem" \
    -subj "/CN=QKD-CA"

# 2. Server key + CSR
openssl genrsa -out "${CERT_DIR}/key.pem" 2048 2>/dev/null
openssl req -new \
    -key "${CERT_DIR}/key.pem" \
    -out "${CERT_DIR}/server.csr" \
    -subj "/CN=${SERVER_IP}"

# 3. Write SAN extensions file
cat > "${CERT_DIR}/_ext.cnf" <<EXTEOF
subjectAltName=IP:${SERVER_IP},IP:127.0.0.1,DNS:localhost
EXTEOF

# 4. Sign with CA + SAN via extfile (works on OpenSSL 1.1.1+)
openssl x509 -req -days 365 \
    -in "${CERT_DIR}/server.csr" \
    -CA "${CERT_DIR}/ca.pem" \
    -CAkey "${CERT_DIR}/ca.key" \
    -CAcreateserial \
    -out "${CERT_DIR}/cert.pem" \
    -extfile "${CERT_DIR}/_ext.cnf"

# 5. Cleanup temp files
rm -f "${CERT_DIR}/server.csr" "${CERT_DIR}/ca.srl" "${CERT_DIR}/_ext.cnf"

echo ""
echo "==> Done! Files in ${CERT_DIR}/"
echo "  ca.pem   - CA certificate (give to curl via --cacert)"
echo "  ca.key   - CA private key"
echo "  cert.pem - Server certificate (give to httplib SSLServer)"
echo "  key.pem  - Server private key (give to httplib SSLServer)"
echo ""
echo "Server start:"
echo "  ./ns3 run curl_qkd_bridge_httplib -- --cert=${CERT_DIR}/cert.pem --key=${CERT_DIR}/key.pem"
echo ""
echo "Client verify:"
echo "  CERT=${CERT_DIR}/ca.pem HOST=${SERVER_IP} ./req_server.sh status"
