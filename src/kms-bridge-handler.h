/*
 * kms-bridge-handler.h
 *
 * ETSI QKD 014 compliant KMS bridge handler for cpp-httplib.
 * Registers REST routes on an httplib::Server (or SSLServer).
 *
 * APIs served:
 *   GET  /api/v1/keys/{master_SAE_ID}/status
 *   GET  /api/v1/keys/{slave_SAE_ID}/enc_keys?number=N&size=S
 *   POST /api/v1/keys/{master_SAE_ID}/dec_keys
 *
 * Usage:
 *   httplib::SSLServer svr(certPath, keyPath);
 *   KmsBridgeHandler handler;
 *   handler.AddKmeEndpoint({ "KME-A", "alice", "bob", "KME-B", kmsA });
 *   handler.AddKmeEndpoint({ "KME-B", "bob", "alice", "KME-A", kmsB });
 *   handler.RegisterRoutes(svr);
 *   svr.listen("0.0.0.0", port);
 */

#pragma once

#include "etsi-qkd-types.h"

#include "ns3/qkd-key-manager-system-application.h"
#include "ns3/qkd-encryptor.h"
#include "ns3/s-buffer.h"
#include "ns3/simulator.h"

#include "httplib.h"

#include <mutex>
#include <string>
#include <vector>
#include <map>

namespace ns3 {

// KME endpoint configuration

struct KmeEndpoint {
    std::string kmeId;          // source_KME_ID
    std::string masterSaeId;    // local SAE identity
    std::string slaveSaeId;     // peer SAE identity
    std::string targetKmeId;    // peer KME identity
    Ptr<QKDKeyManagerSystemApplication> kms;

    // KME capability defaults
    uint16_t maxKeyCount      = 1024;
    uint16_t maxKeyPerRequest = 128;
    uint16_t maxKeySize       = 1024;
    uint16_t minKeySize       = 64;
    uint16_t maxSaeIdCount    = 0;
};

// ETSI QKD 014 bridge handler

class KmsBridgeHandler
{
public:
    KmsBridgeHandler()
    {
        m_encryptor = CreateObject<QKDEncryptor>();
    }

    void AddKmeEndpoint(const KmeEndpoint& ep)
    {
        m_endpoints[ep.masterSaeId] = ep;
    }

    /**
     * Register ETSI QKD 014 routes on the given httplib server.
     * Works with both httplib::Server and httplib::SSLServer.
     */
    template <typename ServerType>
    void RegisterRoutes(ServerType& svr)
    {
        // GET /api/v1/keys/{master_SAE_ID}/status
        svr.Get(R"(/api/v1/keys/([^/]+)/status)",
            [this](const httplib::Request& req, httplib::Response& res) {
                std::string saeId = req.matches[1];
                const KmeEndpoint* ep = FindEndpointBySae(saeId);
                if (!ep) {
                    SetErrorResponse(res, etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT,
                                     "Unknown SAE_ID: " + saeId);
                    return;
                }
                std::lock_guard<std::mutex> lk(m_mtx);
                res.status = etsi::HTTP_STATUS_CODE_SUCCESS;
                res.set_content(GetStatus(*ep), "application/json");
            });

        // GET /api/v1/keys/{slave_SAE_ID}/enc_keys?number=N&size=S
        svr.Get(R"(/api/v1/keys/([^/]+)/enc_keys)",
            [this](const httplib::Request& req, httplib::Response& res) {
                std::string saeId = req.matches[1];
                const KmeEndpoint* ep = FindEndpointBySae(saeId);
                if (!ep) {
                    SetErrorResponse(res, etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT,
                                     "Unknown SAE_ID: " + saeId);
                    return;
                }
                uint32_t number = 1;
                uint16_t size   = etsi::DEFAULT_KEY_SIZE;
                if (req.has_param("number"))
                    number = static_cast<uint32_t>(std::stoul(req.get_param_value("number")));
                if (req.has_param("size"))
                    size = static_cast<uint16_t>(std::stoul(req.get_param_value("size")));

                std::lock_guard<std::mutex> lk(m_mtx);
                auto [code, body] = GetKeys(*ep, number, size);
                res.status = code;
                res.set_content(body, "application/json");
            });

        // POST /api/v1/keys/{master_SAE_ID}/dec_keys
        svr.Post(R"(/api/v1/keys/([^/]+)/dec_keys)",
            [this](const httplib::Request& req, httplib::Response& res) {
                std::string saeId = req.matches[1];
                const KmeEndpoint* ep = FindEndpointBySae(saeId);
                if (!ep) {
                    SetErrorResponse(res, etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT,
                                     "Unknown SAE_ID: " + saeId);
                    return;
                }
                std::lock_guard<std::mutex> lk(m_mtx);
                auto [code, body] = GetKeysWithKeyIds(*ep, req.body);
                res.status = code;
                res.set_content(body, "application/json");
            });

        svr.set_error_handler(
            [](const httplib::Request& /*req*/, httplib::Response& res) {
                // Only set body for unmatched routes (404) where no handler
                // has already written a response.  Without this guard the
                // error handler overwrites legitimate 400/503 messages from
                // route handlers with a misleading "Unknown endpoint" text.
                if (res.body.empty()) {
                    nlohmann::json err;
                    err[etsi::MESSAGE] = "Unknown endpoint";
                    err["endpoints"] = {
                        "GET  /api/v1/keys/{master_SAE_ID}/status",
                        "GET  /api/v1/keys/{slave_SAE_ID}/enc_keys?number=N&size=S",
                        "POST /api/v1/keys/{master_SAE_ID}/dec_keys"
                    };
                    res.set_content(err.dump(2), "application/json");
                }
            });
    }

private:
    std::map<std::string, KmeEndpoint> m_endpoints;  // SAE_ID -> endpoint
    Ptr<QKDEncryptor> m_encryptor;
    mutable std::map<std::string, std::string> m_keyCache; // key_ID -> base64 key
    std::mutex m_mtx;

    // Endpoint resolution

    const KmeEndpoint* FindEndpointBySae(const std::string& saeId) const
    {
        auto it = m_endpoints.find(saeId);
        if (it != m_endpoints.end()) return &it->second;
        // Also check if saeId matches as a slaveSaeId
        for (auto& [key, ep] : m_endpoints) {
            if (ep.slaveSaeId == saeId) return &ep;
        }
        return nullptr;
    }

    // Helpers

    Ptr<SBuffer> FindSBuffer(const std::string& type,
                             Ptr<QKDKeyManagerSystemApplication> kms) const
    {
        if (!kms) return nullptr;
        for (uint32_t id = 0; id < 20; ++id) {
            Ptr<SBuffer> sb = kms->GetSBufferPublic(id, type);
            if (sb) return sb;
        }
        return nullptr;
    }


    void SetErrorResponse(httplib::Response& res, uint16_t code,
                          const std::string& msg) const
    {
        nlohmann::json err;
        err[etsi::MESSAGE] = msg;
        res.status = code;
        res.set_content(err.dump(2), "application/json");
    }

    nlohmann::json BuildKeysArray(const std::vector<Ptr<QKDKey>>& keys) const
    {
        nlohmann::json jkeys = nlohmann::json::array();
        for (const auto& k : keys) {
            if (k) {
                std::string byteKey = k->ConsumeKeyString();
                std::string b64Key  = m_encryptor->Base64Encode(byteKey);
                jkeys.push_back({
                    {etsi::KEY_ID, k->GetId()},
                    {etsi::KEY,    b64Key}
                });
            }
        }
        return jkeys;
    }

    // API: GetStatus

    std::string GetStatus(const KmeEndpoint& ep) const
    {
        nlohmann::json j;

        j[etsi::SOURCE_KME_ID]     = ep.kmeId;
        j[etsi::TARGET_KME_ID]     = ep.targetKmeId;
        j[etsi::MASTER_SAE_ID]     = ep.masterSaeId;
        j[etsi::SLAVE_SAE_ID]      = ep.slaveSaeId;
        j[etsi::MAX_SAE_ID_COUNT]  = ep.maxSaeIdCount;
        j[etsi::MAX_KEY_COUNT]     = ep.maxKeyCount;
        j[etsi::MAX_KEY_PER_REQUEST] = ep.maxKeyPerRequest;
        j[etsi::MAX_KEY_SIZE]      = ep.maxKeySize;
        j[etsi::MIN_KEY_SIZE]      = ep.minKeySize;

        Ptr<SBuffer> buf = FindAnySBuffer(ep.kms);
        if (buf) {
            j[etsi::KEY_SIZE]         = buf->GetKeySize();
            j[etsi::STORED_KEY_COUNT] = buf->GetDefaultKeyCount();
        } else {
            j[etsi::KEY_SIZE]         = etsi::DEFAULT_KEY_SIZE;
            j[etsi::STORED_KEY_COUNT] = 0;
        }

        return j.dump(2);
    }

    // API: GetKeys (enc_keys)

    Ptr<SBuffer> FindAnySBuffer(Ptr<QKDKeyManagerSystemApplication> kms) const
    {
        if (!kms) return nullptr;
        Ptr<SBuffer> sb = FindSBuffer("enc", kms);
        if (sb) return sb;
        return FindSBuffer("dec", kms);
    }

    std::pair<uint16_t, std::string>
    GetKeys(const KmeEndpoint& ep, uint32_t number, uint16_t /*size*/) const
    {
        Ptr<SBuffer> sBuffer = FindAnySBuffer(ep.kms);
        if (!sBuffer) {
            nlohmann::json e;
            e[etsi::MESSAGE] = "SBuffer empty";
            return {etsi::HTTP_STATUS_CODE_SERVER_SIDE_ERROR, e.dump(2)};
        }

        uint32_t available = sBuffer->GetDefaultKeyCount();
        if (available == 0) {
            nlohmann::json e;
            e[etsi::MESSAGE] = "SBuffer empty - no keys available yet";
            return {etsi::HTTP_STATUS_CODE_SERVER_SIDE_ERROR, e.dump(2)};
        }
        if (number > available)
            number = available;

        uint32_t keySize = sBuffer->GetKeySize();
        std::vector<Ptr<QKDKey>> keys;
        for (uint32_t i = 0; i < number; ++i) {
            Ptr<QKDKey> k = sBuffer->GetKey(keySize);
            if (k) keys.push_back(k);
        }

        nlohmann::json jkeys = nlohmann::json::array();
        for (const auto& k : keys) {
            if (k) {
                std::string byteKey = k->ConsumeKeyString();
                std::string b64Key  = m_encryptor->Base64Encode(byteKey);
                std::string keyId   = k->GetId();
                m_keyCache[keyId] = b64Key;
                jkeys.push_back({
                    {etsi::KEY_ID, keyId},
                    {etsi::KEY,    b64Key}
                });
            }
        }

        nlohmann::json j;
        j["keys"] = jkeys;
        return {etsi::HTTP_STATUS_CODE_SUCCESS, j.dump(2)};
    }

    // API: GetKeysWithKeyIds (dec_keys)

    std::pair<uint16_t, std::string>
    GetKeysWithKeyIds(const KmeEndpoint& /*ep*/, const std::string& body) const
    {
        nlohmann::json jbody;
        try { jbody = nlohmann::json::parse(body); }
        catch (...) {
            nlohmann::json e;
            e[etsi::MESSAGE] = "Invalid JSON body";
            return {etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT, e.dump(2)};
        }

        if (!jbody.contains("key_IDs") || !jbody["key_IDs"].is_array()) {
            nlohmann::json e;
            e[etsi::MESSAGE] = "Missing or invalid 'key_IDs' array in request body";
            return {etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT, e.dump(2)};
        }

        nlohmann::json jkeys = nlohmann::json::array();
        for (auto& entry : jbody["key_IDs"]) {
            if (!entry.contains(etsi::KEY_ID)) {
                nlohmann::json e;
                e[etsi::MESSAGE] = "Missing 'key_ID' in key_IDs entry";
                return {etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT, e.dump(2)};
            }
            std::string kid = entry[etsi::KEY_ID];
            auto cacheIt = m_keyCache.find(kid);
            if (cacheIt == m_keyCache.end()) {
                nlohmann::json e;
                e[etsi::MESSAGE] = "key_ID not found: " + kid;
                return {etsi::HTTP_STATUS_CODE_BAD_REQ_FORMAT, e.dump(2)};
            }
            jkeys.push_back({
                {etsi::KEY_ID, kid},
                {etsi::KEY,    cacheIt->second}
            });
            m_keyCache.erase(cacheIt);
        }

        nlohmann::json j;
        j["keys"] = jkeys;
        return {etsi::HTTP_STATUS_CODE_SUCCESS, j.dump(2)};
    }
};

} // namespace ns3
