#!/usr/bin/env bash
set -euo pipefail

# Generate the QKD Root CA (authority) and the end-entity (self) certificate
# used for mutual TLS between the curl client and the QKD bridge server.
#
# Both the server and the client use the same end-entity cert (self.pem) since
# ne_ext.cnf grants both clientAuth and serverAuth EKUs. The server verifies
# client certs against authority.pem; the client verifies the server against
# authority.pem. An arbitrary certificate not signed by this CA is rejected.
#
# Outputs (in this directory):
#   authority.pem       - QKD Root CA certificate (trusted by both sides)
#   authority.key       - QKD Root CA private key (keep secret)
#   self.pem            - end-entity certificate (server + client identity)
#   self.key            - end-entity private key
#   self_combined.pem   - self.key + self.pem (for curl --cert convenience)
#
# Usage:
#   ./gen_qkd_certs.sh
#   ./gen_qkd_certs.sh "/CN=qkd-node/O=QKD-CA"            # custom end-entity subject
#   ./gen_qkd_certs.sh "/CN=qkd-node/O=QKD-CA" 10.7.34.240 # custom subject + server IP for SAN

CERT_DIR="$(cd "$(dirname "$0")" && pwd)"
SELF_SUBJ="${1:-/C=US/ST=Test/L=Test/O=QKD-CA/CN=qkd-node}"
SERVER_IP="${2:-10.7.34.240}"

cd "$CERT_DIR"

echo "==> Generating QKD Root CA (authority) ..."
openssl genrsa -out authority.key 2048 2>/dev/null
openssl req -x509 -new -nodes -key authority.key -out authority.pem \
    -days 3650 -config ca_ext.cnf -extensions v3_ca

echo "==> Generating end-entity (self) certificate signed by the CA ..."
echo "    Server IP for subjectAltName: ${SERVER_IP}"

# Build a per-run ext file with the requested server IP in the SAN, keeping
# the v3_ne extension set from ne_ext.cnf and overriding only the alt_names.
cat > _ne_run.cnf <<EOF
[v3_ne]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = clientAuth, serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName = @alt_names

[alt_names]
DNS.1 = qkd-node
DNS.2 = localhost
IP.1 = 127.0.0.1
IP.2 = ${SERVER_IP}
EOF

openssl genrsa -out self.key 2048 2>/dev/null
openssl req -new -key self.key -out self.csr -subj "$SELF_SUBJ"
openssl x509 -req -in self.csr -CA authority.pem -CAkey authority.key \
    -CAcreateserial -out self.pem -days 365 \
    -extfile _ne_run.cnf -extensions v3_ne
rm -f _ne_run.cnf

# Combined PEM (private key + certificate) for curl --cert convenience.
cat self.key self.pem > self_combined.pem
chmod 600 self.key authority.key self_combined.pem

rm -f self.csr

echo ""
echo "==> Verifying chain ..."
openssl verify -CAfile authority.pem self.pem

echo ""
echo "==> Done. Files in ${CERT_DIR}:"
ls -1 authority.pem authority.key self.pem self.key self_combined.pem

echo ""
echo "Server (run from this directory's parent):"
echo "  ns3 run curl_qkd_bridge -- --cert=certs/self.pem --key=certs/self.key --cacert=certs/authority.pem"
echo ""
echo "Client (HTTPS, mutual TLS):"
echo "  curl --cacert authority.pem --cert self_combined.pem https://localhost:8080/api/v1/keys/any/status"
echo ""
echo "Client (HTTP fallback, no TLS):"
echo "  curl http://localhost:8080/api/v1/keys/any/status"
