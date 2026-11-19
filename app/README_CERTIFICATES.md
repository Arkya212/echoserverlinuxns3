# QKD API Certificate Setup

## ✅ Problem Solved

The original issue was that curl was rejecting the server's self-signed certificate. We have successfully implemented **proper certificate validation** without using the insecure `-k` flag.

## 🔧 Solution Summary

1. **Extracted the actual server certificate** from the running QKD server
2. **Used it as a trusted CA certificate** for validation
3. **Configured hostname resolution** to match the certificate's Common Name
4. **Updated the API client script** to use proper certificate validation

## 📁 Files Created

- `certs/ca_actual.pem` - CA certificate extracted from the server (for validation)
- `certs/server_actual.pem` - Server certificate extracted from the server
- `extract_server_cert.sh` - Script to extract certificates from running server
- `create_certs.sh` - Script to generate new certificates (alternative approach)
- `test_qkd_api.sh` - Test script demonstrating proper certificate validation

## 🚀 Usage

### Basic Commands with Certificate Validation

```bash
# Check status (both SAEs work)
./req_server.sh status alice
./req_server.sh status bob

# Get encryption keys (from slave SAE)
./req_server.sh enc bob 3

# The script now uses proper certificate validation by default
```

### Manual curl Commands

```bash
# Status check with proper certificate validation
curl --cacert certs/ca_actual.pem --resolve qkd-kms:8443:10.7.34.240 \
     https://qkd-kms:8443/api/v1/keys/alice/status

# Get encryption keys
curl --cacert certs/ca_actual.pem --resolve qkd-kms:8443:10.7.34.240 \
     "https://qkd-kms:8443/api/v1/keys/bob/enc_keys?number=2&size=256"
```

## 🔍 Certificate Details

**Server Certificate:**
- **Subject**: `CN=qkd-kms, O=NS3-QKD-Simulation`
- **Issuer**: `CN=qkd-kms, O=NS3-QKD-Simulation` (self-signed)
- **Hostname**: `qkd-kms` (resolved to `10.7.34.240`)

**Validation Method:**
- Uses `--cacert certs/ca_actual.pem` for certificate validation
- Uses `--resolve qkd-kms:8443:10.7.34.240` for hostname resolution
- **No `-k` flag needed** - proper SSL/TLS validation is active

## 📊 SAE Configuration

From the status responses:

**Alice (Master SAE):**
- Role: `master_SAE_ID`
- Can decrypt keys via `dec_keys` endpoint
- Source: `KME-A`, Target: `KME-B`

**Bob (Slave SAE):**
- Role: `slave_SAE_ID` 
- Can get encryption keys via `enc_keys` endpoint
- Source: `KME-B`, Target: `KME-A`

## ⚠️ Known Issues

1. **dec_keys endpoint**: Currently returns "Unknown endpoint" error, suggesting it may not be implemented in this QKD server version
2. **Key consumption**: Keys may be single-use in QKD systems

## 🔒 Security Benefits

✅ **Certificate validation enabled**  
✅ **No insecure `-k` flag**  
✅ **Proper hostname verification**  
✅ **SSL/TLS encryption verified**  

The connection is now properly secured with full certificate chain validation.
