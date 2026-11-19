# QKD Bridge Server - Setup & Usage Guide

## Server Information

| Parameter | Value |
|-----------|-------|
| **KME IP** | `10.7.34.240` |
| **TCP Port** | `8080` |
| **Peer SAE ID** | `alice` |
| **Protocol** | HTTPS (mutual TLS) + HTTP fallback |

---

## Prerequisites: Certificates

To make HTTPS calls to the server, you need two certificate files:

| File | Purpose |
|------|---------|
| `authority.pem` | Self-signed Root CA certificate (trusted by both client and server) |
| `self_combined.pem` | Client certificate + private key (for mutual TLS authentication) |

### Where to Get the Certificates

The certificates are available on the following machine:

| Parameter | Value |
|-----------|-------|
| **Host** | `10.52.244.17` (Hemi) |
| **Path** | `/root/nsim_qkd_certs` |
| **Access** | SSH as `root` (you know the password) |

### Retrieving the Certificates

```bash
# SSH into Hemi to verify the certificates exist
ssh root@10.52.244.17
ls -la /root/nsim_qkd_certs/
# Should show:
#   authority.pem
#   self_combined.pem
exit

# Copy the certificates to your local machine
scp root@10.52.244.17:/root/nsim_qkd_certs/authority.pem ~/nsim_qkd_certs/
scp root@10.52.244.17:/root/nsim_qkd_certs/self_combined.pem ~/nsim_qkd_certs/
```

### Installing the Certificates

Once you have the certificate files, update them in your QKD configuration:

```bash
# Update the Authority (Root CA) certificate
run request security certificate qkd update_authority_from_file /root/nsim_qkd_certs/authority.pem

# Update the Self (client) certificate
run request security certificate qkd update_self_from_file /root/nsim_qkd_certs/self_combined.pem
```

After both certificates are updated successfully, you will be able to make HTTPS calls to the server.

> **Note:** Both certificates use 3072-bit RSA keys. If you see an error like `invalid rsa key size`, the certificates may be outdated. Contact the server administrator to regenerate them.

---

## Verifying Server Connectivity

Before making HTTPS calls, verify that the server is reachable and responding.

### Step 1: Test HTTP Connectivity

```bash
curl http://10.7.34.240:8080/api/v1/keys/alice/status
```

**Expected response** (JSON):
```json
{
  "dec_buffer": {
    "key_count": 66,
    "key_size_bits": 512,
    "total_bits": 33792
  },
  "enc_buffer": {
    "key_count": 160,
    "key_size_bits": 512,
    "total_bits": 116352
  },
  "kms_id": "5db69154-bd7d-11f1-842b-1d91682a4e13",
  "sim_time": 162.5
}
```

If this returns a valid JSON response, the server is running and reachable.

### Step 2: Test HTTPS Connectivity

```bash
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     https://10.7.34.240:8080/api/v1/keys/alice/status
```

This should return the same JSON response as the HTTP test above.

### Troubleshooting

| Error | Cause | Solution |
|-------|-------|----------|
| `Couldn't connect to server` | Server not running or port blocked | Contact the server administrator |
| `certificate is not yet valid` | Clock skew between client and server | Wait a few seconds and retry, or sync clocks |
| `self-signed certificate in certificate chain` | Client has an outdated `authority.pem` | Re-copy `authority.pem` from the cert source |
| `certificate subject name does not match` | Server IP not in certificate SAN | Contact admin to regenerate with correct IP |
| `invalid rsa key size 2048` | Certificates use 2048-bit keys | Contact admin to regenerate with 3072-bit keys |

> **If the call is failing because the server is not reachable, please ping me.**

---

## API Endpoints

### Base URL

```
http://10.7.34.240:8080    (HTTP fallback)
https://10.7.34.240:8080   (HTTPS with mutual TLS)
```

### Available Endpoints

#### 1. Status - Check Buffer Levels

Does **not** consume keys. Safe to call repeatedly.

```bash
# KMS-A (alice)
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     https://10.7.34.240:8080/api/v1/keys/alice/status

# KMS-B (bob)
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     https://10.7.34.240:8080/api/v1/keys/bob/status
```

**Response:**
```json
{
  "kms_id": "5db69154-bd7d-11f1-842b-1d91682a4e13",
  "sim_time": 162.5,
  "enc_buffer": {
    "key_count": 160,
    "key_size_bits": 512,
    "total_bits": 116352
  },
  "dec_buffer": {
    "key_count": 66,
    "key_size_bits": 512,
    "total_bits": 33792
  }
}
```

#### 2. enc_keys - Get Encryption Keys

**Consumes** keys from the encryption SBuffer. Each key is single-use.

**Path-based format:**

```bash
# Get 1 key (default)
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     https://10.7.34.240:8080/api/v1/keys/alice/enc_keys

# Get N keys (e.g. 3)
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     https://10.7.34.240:8080/api/v1/keys/alice/enc_keys/number/3
```

**Query-string format (ETSI 014 standard):**

```bash
# Get 1 key of 256 bits
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     "https://10.7.34.240:8080/api/v1/keys/alice/enc_keys?number=1&size=256"

# Get 3 keys of 256 bits
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     "https://10.7.34.240:8080/api/v1/keys/alice/enc_keys?number=3&size=256"
```

**Response:**
```json
{
  "keys": [
    {
      "key_ID": "DA8DE9F3",
      "key": "base64-encoded-key-material"
    }
  ],
  "source": "ns3-kms",
  "sim_time": 162.5,
  "remaining": 157
}
```

#### 3. dec_keys - Get Decryption Keys by ID

**Consumes** keys from the decryption SBuffer. Requires key IDs obtained from a prior `enc_keys` call.

```bash
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     -X POST https://10.7.34.240:8080/api/v1/keys/bob/dec_keys \
     -H "Content-Type: application/json" \
     -d '{"key_IDs":[{"key_ID":"DA8DE9F3"}]}'
```

**Multiple key IDs at once:**

```bash
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     -X POST https://10.7.34.240:8080/api/v1/keys/bob/dec_keys \
     -H "Content-Type: application/json" \
     -d '{"key_IDs":[{"key_ID":"DA8DE9F3"},{"key_ID":"B7C2E1A9"}]}'
```

**Response:** Same format as `enc_keys`.

---

## SAE Routing

| Path contains | Routes to |
|---------------|-----------|
| `/alice/` | KMS-A (node 3) |
| `/bob/` | KMS-B (node 4) |
| anything else | KMS-A (default) |

---

## Key Behavior Notes

### Key Generation Wait

If the QKD key buffer is empty when an `enc_keys` or `dec_keys` request arrives, the server **holds the request** and prints `Key Generating, please wait...` on the server console. Once keys become available, the request is processed automatically. The client will see the response once keys are served.

- `status` requests are **not** blocked and return immediately with current buffer counts.
- `enc_keys` and `dec_keys` requests **may block** if keys are not yet available.

### Key Consumption

Keys are **single-use**. Each `enc_keys` or `dec_keys` call removes the returned keys from the buffer. Repeated calls will deplete the buffer over time. The `remaining` field in the response shows how many keys are left.

### HTTP Fallback

If you cannot use HTTPS (e.g., no certificates configured), the same endpoints work over plain HTTP:

```bash
curl http://10.7.34.240:8080/api/v1/keys/alice/status
curl http://10.7.34.240:8080/api/v1/keys/alice/enc_keys?number=3&size=256
```

---

## Certificate Summary

### Files Given to the User

| File | Description |
|------|-------------|
| `authority.pem` | Self-signed Root CA certificate |
| `self_combined.pem` | Client private key + client certificate (combined) |

### Files Kept on the Server Only

| File | Description |
|------|-------------|
| `authority.key` | Root CA private key |
| `self.pem` | Client certificate (without key) |
| `self.key` | Client private key (without cert) |
| `machine_cert.pem` | Server TLS certificate |
| `machine_key.pem` | Server TLS private key |

### Certificate Properties

| Certificate | Key Size | Key Usage | Extended Key Usage | Signed By |
|------------|----------|-----------|-------------------|-----------|
| `authority.pem` | 3072-bit RSA | keyCertSign, cRLSign | - | Self-signed |
| `self.pem` | 3072-bit RSA | digitalSignature, keyEncipherment | clientAuth, serverAuth | authority.pem |
| `machine_cert.pem` | 3072-bit RSA | digitalSignature, keyEncipherment | serverAuth | authority.pem |

---

## Quick Reference

```bash
# Verify connectivity (HTTP)
curl http://10.7.34.240:8080/api/v1/keys/alice/status

# Verify connectivity (HTTPS)
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     https://10.7.34.240:8080/api/v1/keys/alice/status

# Get 1 encryption key
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     "https://10.7.34.240:8080/api/v1/keys/alice/enc_keys?number=1&size=256"

# Get 3 encryption keys
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     "https://10.7.34.240:8080/api/v1/keys/alice/enc_keys?number=3&size=256"

# Get decryption key by ID
curl --cacert ~/nsim_qkd_certs/authority.pem \
     --cert ~/nsim_qkd_certs/self_combined.pem \
     -X POST https://10.7.34.240:8080/api/v1/keys/bob/dec_keys \
     -H "Content-Type: application/json" \
     -d '{"key_IDs":[{"key_ID":"DA8DE9F3"}]}'
```
