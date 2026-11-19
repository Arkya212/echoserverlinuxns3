#!/usr/bin/env bash
set -euo pipefail

# Generate a proper mutual-TLS certificate set for curl_qkd_bridge.
#
# Certificate model:
#   1) authority.pem / authority.key
#      - self-signed Root CA
#      - trusted by both client and server
#
#   2) self.pem / self.key / self_combined.pem
#      - client certificate signed by authority.pem
#      - meant to be shared with the user for HTTPS requests
#
#   3) machine_cert.pem / machine_key.pem
#      - server certificate signed by authority.pem
#      - kept on the bridge server only, not shared with the user
#
# Outputs shared with user:
#   authority.pem
#   self_combined.pem
#
# Outputs kept only on server:
#   authority.key
#   self.key
#   self.pem
#   machine_key.pem
#   machine_cert.pem
#
# Usage:
#   ./gen_qkd_certs.sh
#   ./gen_qkd_certs.sh 10.7.34.240
#   ./gen_qkd_certs.sh 10.7.34.240 qkd-kms

CERT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_IP="${1:-10.7.34.240}"
SERVER_DNS="${2:-qkd-kms}"
KEY_BITS="3072"
CLIENT_SUBJ="/C=US/ST=Test/L=Test/O=QKD-CA/CN=qkd-client"
SERVER_SUBJ="/C=US/ST=Test/L=Test/O=QKD-CA/CN=${SERVER_DNS}"

cd "$CERT_DIR"

rm -f authority.pem authority.key authority.srl \
    self.pem self.key self.csr self_combined.pem \
    machine_cert.pem machine_key.pem machine.csr \
    _machine_run.cnf

echo "==> Generating QKD Root CA (authority, ${KEY_BITS}-bit RSA) ..."
openssl genrsa -out authority.key "${KEY_BITS}" 2>/dev/null
openssl req -x509 -new -nodes -key authority.key -out authority.pem \
    -days 3650 -config ca_ext.cnf -extensions v3_ca

echo "==> Generating client certificate (self, ${KEY_BITS}-bit RSA) signed by authority ..."
openssl genrsa -out self.key "${KEY_BITS}" 2>/dev/null
openssl req -new -key self.key -out self.csr -subj "$CLIENT_SUBJ"
openssl x509 -req -in self.csr -CA authority.pem -CAkey authority.key \
    -CAcreateserial -out self.pem -days 365 \
    -extfile ne_ext.cnf -extensions v3_ne
cat self.key self.pem > self_combined.pem

echo "==> Generating server machine certificate (${KEY_BITS}-bit RSA) signed by authority ..."
echo "    Server DNS: ${SERVER_DNS}"
echo "    Server IP : ${SERVER_IP}"
cat > _machine_run.cnf <<EOF
[v3_server]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName = @alt_names

[alt_names]
DNS.1 = ${SERVER_DNS}
DNS.2 = localhost
IP.1 = 127.0.0.1
IP.2 = ${SERVER_IP}
EOF
openssl genrsa -out machine_key.pem "${KEY_BITS}" 2>/dev/null
openssl req -new -key machine_key.pem -out machine.csr -subj "$SERVER_SUBJ"
openssl x509 -req -in machine.csr -CA authority.pem -CAkey authority.key \
    -CAcreateserial -out machine_cert.pem -days 365 \
    -extfile _machine_run.cnf -extensions v3_server

chmod 600 authority.key self.key machine_key.pem self_combined.pem
rm -f self.csr machine.csr _machine_run.cnf

echo ""
echo "==> Verifying chains ..."
openssl verify -CAfile authority.pem self.pem
openssl verify -CAfile authority.pem machine_cert.pem

echo ""
echo "==> Files created in ${CERT_DIR}:"
ls -1 authority.pem authority.key self.pem self.key self_combined.pem machine_cert.pem machine_key.pem

echo ""
echo "==> Share with user:"
echo "  authority.pem"
echo "  self_combined.pem"
echo ""
echo "==> QKD profile note: authority/self/machine keys are generated as ${KEY_BITS}-bit RSA."
echo ""
echo "==> Keep only on server:"
echo "  authority.key"
echo "  self.pem"
echo "  self.key"
echo "  machine_cert.pem"
echo "  machine_key.pem"
echo ""
echo "Server start:"
echo "  ns3 run curl_qkd_bridge -- --cert=certs/machine_cert.pem --key=certs/machine_key.pem --cacert=certs/authority.pem"
echo ""
echo "Client (HTTPS, mutual TLS):"
echo "  curl --cacert authority.pem --cert self_combined.pem https://${SERVER_IP}:8080/api/v1/keys/any/status"
echo ""
echo "Client (HTTP fallback, no TLS):"
echo "  curl http://${SERVER_IP}:8080/api/v1/keys/any/status"
