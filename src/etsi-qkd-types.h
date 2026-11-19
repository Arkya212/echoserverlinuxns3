/*
 * etsi-qkd-types.h
 *
 * ETSI QKD 014 compliant types, constants, JSON field names,
 * request/response structures, and status codes.
 *
 * Reference: ETSI GS QKD 014 V1.1.1 (2019-02)
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace etsi {

// ── Constants ────────────────────────────────────────────────────────────────

const uint16_t DEFAULT_KEY_SIZE       = 256;
const uint16_t HTTPS_PORT_443         = 443;
const std::string HTTPS_PREFIX        = "https://";
const std::string VERSION             = "v1";

// ── HTTP Status Codes ────────────────────────────────────────────────────────

const uint16_t HTTP_STATUS_CODE_SUCCESS              = 200;
const uint16_t HTTP_STATUS_CODE_BAD_REQ_FORMAT       = 400;
const uint16_t HTTP_STATUS_CODE_AUTHORIZATION_FAILED = 401;
const uint16_t HTTP_STATUS_CODE_SERVER_SIDE_ERROR    = 503;
const uint16_t HTTP_STATUS_CODE_INVALID              = 0;
const uint16_t INVALID_TCP_PORT                      = 0;

// ── JSON Field Names ─────────────────────────────────────────────────────────

const char* const KEY_SIZE            = "key_size";
const char* const MASTER_SAE_ID      = "master_SAE_ID";
const char* const MAX_SAE_ID_COUNT   = "max_SAE_ID_count";
const char* const MAX_KEY_COUNT      = "max_key_count";
const char* const MAX_KEY_PER_REQUEST = "max_key_per_request";
const char* const MAX_KEY_SIZE       = "max_key_size";
const char* const MIN_KEY_SIZE       = "min_key_size";
const char* const SLAVE_SAE_ID       = "slave_SAE_ID";
const char* const SOURCE_KME_ID     = "source_KME_ID";
const char* const STORED_KEY_COUNT  = "stored_key_count";
const char* const TARGET_KME_ID     = "target_KME_ID";
const char* const KEY                = "key";
const char* const KEY_ID             = "key_ID";
const char* const MESSAGE            = "message";

// ── Key Types ────────────────────────────────────────────────────────────────

using Key   = std::string;   // Base64-encoded quantum key material
using KeyId = std::string;   // Unique key identifier string

// ── Certificate Parameters ───────────────────────────────────────────────────

struct CertParams {
    std::string m_CaCert;       // CA certificate path
    std::string m_ClientCert;   // Client certificate path
    std::string m_ClientKey;    // Client private key path
};

// ── Entity Parameters ────────────────────────────────────────────────────────

struct EntityParams {
    std::string   m_KmeIpAddr;
    std::string   m_PeerSaeId;
    std::uint16_t m_TcpPortNum;

    EntityParams() : m_TcpPortNum(INVALID_TCP_PORT) {}

    EntityParams(const std::string& a_KmeIpAddr,
                 const std::string& a_PeerSaeId,
                 const std::uint16_t& a_TcpPortNum)
        : m_KmeIpAddr(a_KmeIpAddr)
        , m_PeerSaeId(a_PeerSaeId)
        , m_TcpPortNum(a_TcpPortNum)
    {}
};

// ── Common Response Parameters ───────────────────────────────────────────────

struct CommonRespParams {
    uint16_t    m_HttpCode;
    std::string m_HttpMessage;

    CommonRespParams() : m_HttpCode(HTTP_STATUS_CODE_INVALID) {}
};

// ── Status Request/Response ──────────────────────────────────────────────────

struct StatusReqParams {
    std::string  m_KmeIpAddr;
    uint16_t     m_TcpPortNum;
    std::string  m_MasterSaeId;
    CertParams   m_Certs;

    StatusReqParams() : m_TcpPortNum(INVALID_TCP_PORT) {}
};

struct StatusRespParams {
    CommonRespParams m_CommonRespParams;
    uint16_t     m_KeySize;
    std::string  m_MasterSaeId;
    uint16_t     m_MaxSaeIdCount;
    uint16_t     m_MaxKeyCount;
    uint16_t     m_MaxKeyPerRequest;
    uint16_t     m_MinKeySize;
    uint16_t     m_MaxKeySize;
    std::string  m_SlaveSaeId;
    std::string  m_SourceKmeId;
    std::string  m_TargetKmeId;
    uint16_t     m_StoredKeyCount;

    StatusRespParams()
        : m_KeySize(DEFAULT_KEY_SIZE)
        , m_MaxSaeIdCount(0)
        , m_MaxKeyCount(0)
        , m_MaxKeyPerRequest(0)
        , m_MinKeySize(0)
        , m_MaxKeySize(0)
        , m_StoredKeyCount(0)
    {}
};

// ── Key Request/Response ─────────────────────────────────────────────────────

struct KeyRequestParams {
    EntityParams       m_EntityParams;
    uint16_t           m_NumKeys;
    uint16_t           m_KeySize;
    std::vector<KeyId> m_KeyIds;    // For dec_keys
    CertParams         m_Certs;

    KeyRequestParams() : m_NumKeys(1), m_KeySize(DEFAULT_KEY_SIZE) {}
};

struct KeyResponseParams {
    CommonRespParams        m_CommonRespParams;
    std::map<KeyId, Key>    m_Keys;
};

} // namespace etsi
