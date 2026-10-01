/*
 * QKD Key Retrieval via curl - bridges real TCP socket to NS-3 KMS SBuffers
 *
 * Flow: curl -> Real Socket -> NS-3 KMS (SBuffer lookup) -> Real Socket -> curl
 *
 * Transport security:
 *   The server multiplexes HTTPS (mutual TLS) and plain HTTP on the same port
 *   by peeking the first byte of every accepted connection. A TLS record
 *   starts with 0x16 (handshake); anything else is treated as plain HTTP.
 *
 *   - HTTPS path: the client MUST present a certificate signed by the trusted
 *     QKD Root CA (authority.pem). If the certificate is missing or does not
 *     validate, the TLS handshake is rejected and the connection is dropped
 *     with a console message. An arbitrary client cannot use HTTPS.
 *   - HTTP path: plain HTTP connections are still served (fallback). This lets
 *     simple clients retrieve keys without TLS, while HTTPS remains gated by
 *     mutual certificate validation.
 *
 *   If the server's own certificate/key/CA files cannot be loaded at startup,
 *   the server falls back to HTTP-only mode (no TLS at all).
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mobility-module.h"
#include "ns3/qkd-app-014.h"
#include "ns3/qkd-app-helper.h"
#include "ns3/qkd-link-helper.h"
#include "ns3/qkd-control.h"
#include "ns3/qkd-key-manager-system-application.h"
#include "ns3/qkd-encryptor.h"
#include "ns3/s-buffer.h"
#include "ns3/q-buffer.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <sstream>
#include <chrono>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CurlQKDBridge");

struct CurlRequest
{
    int clientFd;
    SSL* ssl;            // nullptr -> plain HTTP on clientFd; non-null -> HTTPS
    std::string method;
    std::string path;
    std::string body;
};

struct CurlResponse
{
    int clientFd;
    SSL* ssl;
    int statusCode;
    std::string body;
};

std::queue<CurlRequest>  g_reqQueue;
std::queue<CurlResponse> g_respQueue;
std::mutex               g_reqMtx, g_respMtx;
std::condition_variable  g_respCV;
std::atomic<bool>        g_running(true);

Ptr<QKDKeyManagerSystemApplication> g_kmsA = nullptr;
Ptr<QKDKeyManagerSystemApplication> g_kmsB = nullptr;
Ptr<QKDEncryptor> g_encryptor = nullptr;

// TLS context (nullptr when running in HTTP-only fallback mode).
SSL_CTX* g_sslCtx = nullptr;
std::string g_certFile = "/auto/bausers/aaditya/work/ns3_sim/ns-allinone-3.42/ns-3.42/contrib/echoserverlinuxns3/tasks/certs/self.pem";
std::string g_keyFile  = "/auto/bausers/aaditya/work/ns3_sim/ns-allinone-3.42/ns-3.42/contrib/echoserverlinuxns3/tasks/certs/self.key";
std::string g_caFile   = "/auto/bausers/aaditya/work/ns3_sim/ns-allinone-3.42/ns-3.42/contrib/echoserverlinuxns3/tasks/certs/authority.pem";

// Throttles the "Key Generating" log so it prints once per wait episode.
std::atomic<bool> g_keyWaitLogged(false);

Ptr<SBuffer> FindSBuffer(const std::string& type, Ptr<QKDKeyManagerSystemApplication> kms)
{
    if (!kms) return nullptr;
    for (uint32_t id = 0; id < 20; ++id) {
        Ptr<SBuffer> sb = kms->GetSBufferPublic(id, type);
        if (sb) return sb;
    }
    return nullptr;
}

std::string BuildKeyJson(const std::vector<Ptr<QKDKey>>& keys, Ptr<SBuffer> sBuffer)
{
    if (!g_encryptor)
        g_encryptor = CreateObject<QKDEncryptor>();

    nlohmann::json jkeys;
    for (uint32_t i = 0; i < keys.size(); ++i) {
        if (keys[i]) {
            std::string byteKey = keys[i]->ConsumeKeyString();
            std::string b64Key  = g_encryptor->Base64Encode(byteKey);
            jkeys["keys"].push_back({
                {"key_ID", keys[i]->GetId()},
                {"key",    b64Key}
            });
        }
    }
    jkeys["source"]    = "ns3-kms";
    jkeys["sim_time"]  = Simulator::Now().GetSeconds();
    jkeys["remaining"] = sBuffer ? sBuffer->GetDefaultKeyCount() : 0;
    return jkeys.dump(2);
}

// Returns true if the QKD SBuffer for the given type/KMS has keys ready,
// i.e. the buffer exists and holds at least one key.
static bool KeysReady(const std::string& type, Ptr<QKDKeyManagerSystemApplication> kms)
{
    Ptr<SBuffer> sb = FindSBuffer(type, kms);
    return sb && sb->GetDefaultKeyCount() > 0;
}

class KmsBridgeApp : public Application
{
public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("KmsBridgeApp")
            .SetParent<Application>()
            .SetGroupName("Applications")
            .AddConstructor<KmsBridgeApp>();
        return tid;
    }
    KmsBridgeApp() {}
    virtual ~KmsBridgeApp() {}

private:
    EventId m_pollEvent;
    // Requests waiting for QKD keys to be generated (Task 3). Touched only
    // from the simulator thread, so no mutex is required.
    std::queue<CurlRequest> m_waitQueue;
    EventId m_waitEvent;

    void StartApplication() override
    {
        m_pollEvent = Simulator::Schedule(MilliSeconds(50),
                                          &KmsBridgeApp::PollRequests, this);
    }

    void StopApplication() override
    {
        Simulator::Cancel(m_pollEvent);
        Simulator::Cancel(m_waitEvent);
    }

    void PollRequests()
    {
        std::unique_lock<std::mutex> lk(g_reqMtx);
        while (!g_reqQueue.empty()) {
            CurlRequest req = g_reqQueue.front();
            g_reqQueue.pop();
            lk.unlock();
            HandleRequest(req);
            lk.lock();
        }
        EnsureRetry();
        m_pollEvent = Simulator::Schedule(MilliSeconds(50),
                                          &KmsBridgeApp::PollRequests, this);
    }

    // Park a request until QKD keys are available, then retry it on a
    // simulator schedule (Task 3). Using a separate member queue (instead
    // of pushing back into g_reqQueue) avoids a tight busy-loop inside
    // PollRequests that would stall the simulator and prevent key
    // generation from ever progressing.
    void ParkForKeys(const CurlRequest& req)
    {
        m_waitQueue.push(req);
    }

    // Make sure exactly one retry event is pending whenever there are parked
    // requests. Called after every poll/retry pass so it never double-books.
    void EnsureRetry()
    {
        if (!m_waitQueue.empty() && !m_waitEvent.IsRunning()) {
            m_waitEvent = Simulator::Schedule(MilliSeconds(50),
                                              &KmsBridgeApp::RetryWaiting,
                                              this);
        }
    }

    // Re-examine parked requests once keys may have been generated. Ready
    // requests are processed (and answered); still-empty ones are parked
    // again and the retry is rescheduled.
    void RetryWaiting()
    {
        std::queue<CurlRequest> pending;
        pending.swap(m_waitQueue);
        while (!pending.empty()) {
            CurlRequest req = pending.front();
            pending.pop();
            HandleRequest(req);
        }
        EnsureRetry();
    }

    void HandleRequest(const CurlRequest& req)
    {
        NS_LOG_INFO("Processing: " << req.method << " " << req.path
                    << " at sim-time " << Simulator::Now().GetSeconds() << "s"
                    << (req.ssl ? " [HTTPS]" : " [HTTP]"));

        std::string respBody;
        int status = 200;

        // Determine which KMS based on path (/alice or /bob)
        Ptr<QKDKeyManagerSystemApplication> targetKms = g_kmsA;
        if (req.path.find("/bob/") != std::string::npos) {
            targetKms = g_kmsB;
        }

        bool isEncKeys = req.path.find("/enc_keys") != std::string::npos;
        bool isDecKeys = req.path.find("/dec_keys") != std::string::npos;

        // Task 3: key-fetch endpoints must wait until keys are available.
        if (isEncKeys || isDecKeys) {
            const std::string bufType = isEncKeys
                ? std::string("enc") : std::string("dec");
            if (!KeysReady(bufType, targetKms)) {
                if (!g_keyWaitLogged.exchange(true)) {
                    NS_LOG_UNCOND("Key Generating, please wait...");
                }
                ParkForKeys(req);
                return;
            }
            if (g_keyWaitLogged.exchange(false)) {
                NS_LOG_UNCOND("Keys ready, processing request.");
            }
        }

        if (isEncKeys) {
            respBody = DoGetKeys(req, targetKms);
        } else if (isDecKeys) {
            respBody = DoGetKeysByIds(req, targetKms);
        } else if (req.path.find("/status") != std::string::npos) {
            respBody = DoStatus(targetKms);
        } else {
            status = 404;
            nlohmann::json err;
            err["error"] = "Unknown endpoint";
            err["endpoints"] = {
                "GET  /api/v1/keys/alice/status  (KMS-A)",
                "GET  /api/v1/keys/bob/status    (KMS-B)",
                "GET  /api/v1/keys/alice/enc_keys/number/<N>",
                "POST /api/v1/keys/bob/dec_keys"
            };
            respBody = err.dump(2);
        }

        std::lock_guard<std::mutex> lk(g_respMtx);
        g_respQueue.push({req.clientFd, req.ssl, status, respBody});
        g_respCV.notify_one();
    }

    // GET /enc_keys - fetch keys from specified KMS enc SBuffer
    std::string DoGetKeys(const CurlRequest& req, Ptr<QKDKeyManagerSystemApplication> kms)
    {
        Ptr<SBuffer> sBuffer = FindSBuffer("enc", kms);
        if (!sBuffer)
            return "{\"error\": \"No enc SBuffer found - keys not generated yet\"}";

        uint32_t keyNumber = 1;
        size_t numPos = req.path.find("/number/");
        if (numPos != std::string::npos) {
            size_t start = numPos + 8;
            size_t end = req.path.find("/", start);
            keyNumber = std::stoi(req.path.substr(start, end == std::string::npos ? end : end - start));
        }

        uint32_t available = sBuffer->GetDefaultKeyCount();
        if (available == 0)
            return "{\"error\": \"SBuffer empty - no keys available yet\"}";
        if (keyNumber > available)
            keyNumber = available;

        uint32_t keySize = sBuffer->GetKeySize();
        std::vector<Ptr<QKDKey>> keys;
        for (uint32_t i = 0; i < keyNumber; ++i) {
            Ptr<QKDKey> k = sBuffer->GetKey(keySize);
            if (k) keys.push_back(k);
        }
        return BuildKeyJson(keys, sBuffer);
    }

    // POST /dec_keys - fetch keys by ID from specified KMS dec SBuffer
    std::string DoGetKeysByIds(const CurlRequest& req, Ptr<QKDKeyManagerSystemApplication> kms)
    {
        Ptr<SBuffer> sBuffer = FindSBuffer("dec", kms);
        if (!sBuffer)
            return "{\"error\": \"No dec SBuffer found\"}";

        nlohmann::json jbody;
        try { jbody = nlohmann::json::parse(req.body); }
        catch (...) { return "{\"error\": \"Invalid JSON body\"}"; }

        std::vector<Ptr<QKDKey>> keys;
        for (auto& entry : jbody["key_IDs"]) {
            std::string kid = entry["key_ID"];
            Ptr<QKDKey> k = sBuffer->GetKey(kid, false);
            if (!k) {
                nlohmann::json e;
                e["error"]  = "key_ID not found";
                e["key_ID"] = kid;
                return e.dump(2);
            }
            keys.push_back(k);
        }
        return BuildKeyJson(keys, sBuffer);
    }

    // GET /status - report buffer levels
    std::string DoStatus(Ptr<QKDKeyManagerSystemApplication> kms)
    {
        nlohmann::json j;
        j["kms_id"] = kms ? kms->GetId() : "N/A";
        j["sim_time"] = Simulator::Now().GetSeconds();

        Ptr<SBuffer> encBuf = FindSBuffer("enc", kms);
        Ptr<SBuffer> decBuf = FindSBuffer("dec", kms);

        if (encBuf) {
            j["enc_buffer"]["key_count"]     = encBuf->GetDefaultKeyCount();
            j["enc_buffer"]["key_size_bits"] = encBuf->GetKeySize();
            j["enc_buffer"]["total_bits"]    = encBuf->GetBitCount();
        } else {
            j["enc_buffer"] = "not available yet";
        }
        if (decBuf) {
            j["dec_buffer"]["key_count"]     = decBuf->GetDefaultKeyCount();
            j["dec_buffer"]["key_size_bits"] = decBuf->GetKeySize();
            j["dec_buffer"]["total_bits"]    = decBuf->GetBitCount();
        } else {
            j["dec_buffer"] = "not available yet";
        }
        return j.dump(2);
    }
};

NS_OBJECT_ENSURE_REGISTERED(KmsBridgeApp);

// Build the TLS server context. Loads the server cert/key and the CA used to
// verify client certificates. Returns nullptr on any failure (HTTP fallback).
static SSL_CTX* InitSslContext()
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        NS_LOG_UNCOND("SSL_CTX_new failed: " << ERR_error_string(ERR_get_error(), nullptr));
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);

    if (SSL_CTX_use_certificate_file(ctx, g_certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        NS_LOG_UNCOND("Failed to load server certificate (" << g_certFile << "): "
                      << ERR_error_string(ERR_get_error(), nullptr));
        SSL_CTX_free(ctx);
        return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, g_keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        NS_LOG_UNCOND("Failed to load server private key (" << g_keyFile << "): "
                      << ERR_error_string(ERR_get_error(), nullptr));
        SSL_CTX_free(ctx);
        return nullptr;
    }
    if (SSL_CTX_load_verify_locations(ctx, g_caFile.c_str(), nullptr) <= 0) {
        NS_LOG_UNCOND("Failed to load CA (" << g_caFile << "): "
                      << ERR_error_string(ERR_get_error(), nullptr));
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Require and verify the client certificate against the trusted CA.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    SSL_CTX_set_client_CA_list(ctx, SSL_load_client_CA_file(g_caFile.c_str()));

    return ctx;
}

// Read a complete HTTP request. Uses SSL_read when ssl != nullptr, read() for
// plain HTTP. Handles Content-Length to ensure the full body is received.
static bool ReadHttpRequest(int fd, SSL* ssl, std::string& raw)
{
    char buf[8192] = {};
    ssize_t nr;
    if (ssl) {
        nr = SSL_read(ssl, buf, sizeof(buf) - 1);
    } else {
        nr = read(fd, buf, sizeof(buf) - 1);
    }
    if (nr <= 0) return false;
    raw.append(buf, nr);

    // Check if we have Content-Length and need to read more
    size_t clPos = raw.find("Content-Length: ");
    if (clPos != std::string::npos) {
        size_t clEnd = raw.find("\r\n", clPos);
        int contentLen = std::stoi(raw.substr(clPos + 16, clEnd - clPos - 16));
        size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            int bodyReceived = static_cast<int>(raw.size() - (headerEnd + 4));
            while (bodyReceived < contentLen) {
                if (ssl) {
                    nr = SSL_read(ssl, buf, sizeof(buf) - 1);
                } else {
                    nr = read(fd, buf, sizeof(buf) - 1);
                }
                if (nr <= 0) break;
                raw.append(buf, nr);
                bodyReceived += static_cast<int>(nr);
            }
        }
    }
    return true;
}

void TcpAcceptThread(uint16_t port)
{
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(serverFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(serverFd);
        return;
    }
    listen(serverFd, 10);

    NS_LOG_UNCOND("");
    if (g_sslCtx) {
        NS_LOG_UNCOND("=== QKD Key Server (HTTPS + HTTP fallback) on port "
                      << port << " ===");
        NS_LOG_UNCOND("Mutual TLS: client cert verified against "
                      << g_caFile);
    } else {
        NS_LOG_UNCOND("=== QKD Key Server (HTTP-only fallback) on port "
                      << port << " ===");
        NS_LOG_UNCOND("TLS disabled - server certificates not loaded.");
    }
    NS_LOG_UNCOND("Endpoints:");
    NS_LOG_UNCOND("  GET  http(s)://<HOST>:" << port << "/api/v1/keys/any/status");
    NS_LOG_UNCOND("  GET  http(s)://<HOST>:" << port << "/api/v1/keys/any/enc_keys");
    NS_LOG_UNCOND("  GET  http(s)://<HOST>:" << port << "/api/v1/keys/any/enc_keys/number/3");
    NS_LOG_UNCOND("  POST http(s)://<HOST>:" << port << "/api/v1/keys/any/dec_keys");
    NS_LOG_UNCOND("");

    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(serverFd, &fds);
        timeval tv{1, 0};
        if (select(serverFd + 1, &fds, nullptr, nullptr, &tv) <= 0)
            continue;

        sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(serverFd, (sockaddr*)&caddr, &clen);
        if (cfd < 0) continue;

        // Peek the first byte to detect TLS (0x16 = TLS handshake) vs HTTP.
        SSL* ssl = nullptr;
        if (g_sslCtx) {
            char firstByte = 0;
            ssize_t peeked = recv(cfd, &firstByte, 1, MSG_PEEK);
            if (peeked <= 0) { close(cfd); continue; }

            if (static_cast<unsigned char>(firstByte) == 0x16) {
                // TLS connection - require a valid client certificate.
                ssl = SSL_new(g_sslCtx);
                if (!ssl) { close(cfd); continue; }
                SSL_set_fd(ssl, cfd);

                if (SSL_accept(ssl) <= 0) {
                    long vrf = SSL_get_verify_result(ssl);
                    if (vrf == X509_V_OK) {
                        NS_LOG_UNCOND("HTTPS rejected: client certificate "
                                      "NOT found or handshake failed.");
                    } else {
                        NS_LOG_UNCOND("HTTPS rejected: client certificate "
                                      "did not match ("
                                      << X509_verify_cert_error_string(vrf)
                                      << ").");
                    }
                    ERR_clear_error();
                    SSL_free(ssl);
                    close(cfd);
                    continue;
                }

                X509* peer = SSL_get_peer_certificate(ssl);
                if (!peer) {
                    NS_LOG_UNCOND("HTTPS rejected: client certificate "
                                  "NOT found - connection dropped.");
                    SSL_free(ssl);
                    close(cfd);
                    continue;
                }
                X509_free(peer);
            }
        }

        std::string raw;
        if (!ReadHttpRequest(cfd, ssl, raw)) {
            if (ssl) { SSL_free(ssl); }
            close(cfd);
            continue;
        }

        std::string method = raw.substr(0, raw.find(' '));
        size_t ps = raw.find(' ') + 1;
        size_t pe = raw.find(' ', ps);
        std::string path = raw.substr(ps, pe - ps);

        std::string body;
        size_t bodyStart = raw.find("\r\n\r\n");
        if (bodyStart != std::string::npos)
            body = raw.substr(bodyStart + 4);

        {
            std::lock_guard<std::mutex> lk(g_reqMtx);
            g_reqQueue.push({cfd, ssl, method, path, body});
        }
    }
    close(serverFd);
}

void TcpResponseThread()
{
    while (g_running) {
        std::unique_lock<std::mutex> lk(g_respMtx);
        g_respCV.wait_for(lk, std::chrono::milliseconds(200),
                          [] { return !g_respQueue.empty() || !g_running; });

        while (!g_respQueue.empty()) {
            CurlResponse r = g_respQueue.front();
            g_respQueue.pop();
            lk.unlock();

            std::ostringstream http;
            http << "HTTP/1.1 " << r.statusCode << " OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << r.body.size() << "\r\n"
                 << "Connection: close\r\n\r\n"
                 << r.body;
            std::string s = http.str();
            if (r.ssl) {
                SSL_write(r.ssl, s.c_str(),
                          static_cast<int>(s.size()));
                SSL_shutdown(r.ssl);
                SSL_free(r.ssl);
            } else {
                ssize_t wr = write(r.clientFd, s.c_str(), s.size());
                (void)wr;
            }
            close(r.clientFd);

            lk.lock();
        }
    }
}

int main(int argc, char* argv[])
{
    /*
    Here, we can't use a port that is already being used by any other process
    ss -tulnp
    lists all the processes that are listening on diff ports. If we use any Port
    that is already listed in the output, we get a bind error.
    [TODO: This change has not been pushed to remote repo, please look into it later]

    One imp thing is: the curlPort is a TCP Port, but it can share the same socket number
    as a UDP port. Apparently, diff protocols can share same port numbers
    */
    uint16_t curlPort = 8080; //This port is not being used by std httpd of Apache Server so its ok to use this
    double simTime = 600.0;
    uint32_t ppKeySize = 512;
    std::string ppKeyRate = "100kbps";
    uint32_t ppPacketSize = 1000;
    std::string ppRate = "1Mbps";
    uint32_t numberOfKeyToFetchFromKMS = 3;

    CommandLine cmd(__FILE__);
    cmd.AddValue("port", "HTTP/HTTPS port for curl access", curlPort);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("cert", "Server TLS certificate (PEM)", g_certFile);
    cmd.AddValue("key", "Server TLS private key (PEM)", g_keyFile);
    cmd.AddValue("cacert", "CA cert to verify client certs (PEM)", g_caFile);
    cmd.Parse(argc, argv);

    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::RealtimeSimulatorImpl"));

    // Initialize OpenSSL and the TLS context. On any failure the server
    // transparently falls back to HTTP-only mode.
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    g_sslCtx = InitSslContext();
    if (g_sslCtx) {
        NS_LOG_UNCOND("TLS enabled: cert=" << g_certFile
                      << " key=" << g_keyFile
                      << " cacert=" << g_caFile);
    } else {
        NS_LOG_UNCOND("TLS disabled - serving HTTP only (fallback).");
    }

    // Create 9 nodes (same topology as etsi_014 example)
    NodeContainer n;
    n.Create(9);
    // node 0: app-A, node 1: relay, node 2: app-B
    // node 3: KMS-A, node 4: KMS-B
    // node 5: ctrl-A, node 6: ctrl-B
    // node 7: PP-slave, node 8: PP-master

    NodeContainer n0n1(n.Get(0), n.Get(1));
    NodeContainer n1n2(n.Get(1), n.Get(2));
    NodeContainer n0n3(n.Get(0), n.Get(3));
    NodeContainer n7n3(n.Get(7), n.Get(3));
    NodeContainer n2n4(n.Get(2), n.Get(4));
    NodeContainer n8n4(n.Get(8), n.Get(4));
    NodeContainer n3n4(n.Get(3), n.Get(4));

    InternetStackHelper internetHelper;
    internetHelper.Install(n);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator>();
    posAlloc->Add(Vector(0,   0,   0));
    posAlloc->Add(Vector(200, 0,   0));
    posAlloc->Add(Vector(400, 0,   0));
    posAlloc->Add(Vector(0,   100, 0));
    posAlloc->Add(Vector(400, 100, 0));
    posAlloc->Add(Vector(0,   200, 0));
    posAlloc->Add(Vector(400, 200, 0));
    posAlloc->Add(Vector(0,   50,  0));
    posAlloc->Add(Vector(400, 50,  0));
    mobility.SetPositionAllocator(posAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(n);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("50Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    NetDeviceContainer d0d1 = p2p.Install(n0n1);
    NetDeviceContainer d1d2 = p2p.Install(n1n2);
    NetDeviceContainer d0d3 = p2p.Install(n0n3);
    NetDeviceContainer d7d3 = p2p.Install(n7n3);
    NetDeviceContainer d2d4 = p2p.Install(n2n4);
    NetDeviceContainer d8d4 = p2p.Install(n8n4);
    NetDeviceContainer d3d4 = p2p.Install(n3n4);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer i0i1 = ipv4.Assign(d0d1);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer i1i2 = ipv4.Assign(d1d2);
    ipv4.SetBase("10.1.3.0", "255.255.255.0");
    Ipv4InterfaceContainer i0i3 = ipv4.Assign(d0d3);
    ipv4.SetBase("10.1.4.0", "255.255.255.0");
    Ipv4InterfaceContainer i2i4 = ipv4.Assign(d2d4);
    ipv4.SetBase("10.1.5.0", "255.255.255.0");
    Ipv4InterfaceContainer i3i4 = ipv4.Assign(d3d4);
    ipv4.SetBase("10.1.6.0", "255.255.255.0");
    Ipv4InterfaceContainer i7i3 = ipv4.Assign(d7d3);
    ipv4.SetBase("10.1.7.0", "255.255.255.0");
    Ipv4InterfaceContainer i8i4 = ipv4.Assign(d8d4);

    // Install QKD controllers and KMS
    QKDAppHelper QAHelper;
    QKDLinkHelper QLinkHelper;

    Ptr<QKDControl> ctrlA = QLinkHelper.InstallQKDNController(n.Get(5));
    Ptr<QKDControl> ctrlB = QLinkHelper.InstallQKDNController(n.Get(6));
    QLinkHelper.ConfigureQBuffers({ctrlA, ctrlB}, 1024, 1800, 500000000, 512);

    QAHelper.InstallKeyManager(n.Get(3), i3i4.GetAddress(0), 80, ctrlA);
    QAHelper.InstallKeyManager(n.Get(4), i3i4.GetAddress(1), 80, ctrlB);

    // Grab KMS pointers for the bridge
    g_kmsA = n.Get(3)->GetApplication(0)->GetObject<QKDKeyManagerSystemApplication>();
    g_kmsB = n.Get(4)->GetApplication(0)->GetObject<QKDKeyManagerSystemApplication>();

    NS_LOG_UNCOND("KMS-A on node 3, IP: " << i3i4.GetAddress(0));
    NS_LOG_UNCOND("KMS-B on node 4, IP: " << i3i4.GetAddress(1));

    // Post-processing applications (key generation)
    ApplicationContainer ppApps;
    ppApps.Add(QAHelper.InstallPostProcessing(
        n.Get(0), n.Get(2),
        InetSocketAddress(i0i1.GetAddress(0), 102),
        InetSocketAddress(i1i2.GetAddress(1), 102),
        n.Get(5), n.Get(6),
        ppKeySize,
        DataRate(ppKeyRate),
        ppPacketSize,
        DataRate(ppRate)
    ));
    ppApps.Start(Seconds(1.0));
    ppApps.Stop(Seconds(simTime - 5));

    // Crypto applications (consume keys from KMS)
    Config::SetDefault("ns3::QKDApp014::NumberOfKeyToFetchFromKMS",
                       UintegerValue(numberOfKeyToFetchFromKMS));
    Config::SetDefault("ns3::QKDApp014::AuthenticationType", UintegerValue(1));
    Config::SetDefault("ns3::QKDApp014::EncryptionType", UintegerValue(2));
    Config::SetDefault("ns3::QKDApp014::AESLifetime", UintegerValue(10000));
    Config::SetDefault("ns3::QKDApp014::UseCrypto", UintegerValue(1));

    uint16_t commPort = 8081;
    ApplicationContainer cryptoApps;
    cryptoApps.Add(QAHelper.InstallQKDApplication(
        n.Get(0), n.Get(2),
        InetSocketAddress(i0i1.GetAddress(0), commPort),
        InetSocketAddress(i1i2.GetAddress(1), commPort),
        n.Get(5), n.Get(6),
        "tcp", 1000, DataRate("100kbps"), "etsi014"
    ));
    cryptoApps.Start(Seconds(5.0));
    cryptoApps.Stop(Seconds(simTime - 5));

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    QLinkHelper.CreateTopologyGraph({ctrlA, ctrlB});
    QLinkHelper.PopulateRoutingTables();
    QLinkHelper.AddGraphs();

    // Install the bridge app on KMS-A node
    Ptr<KmsBridgeApp> bridge = CreateObject<KmsBridgeApp>();
    n.Get(3)->AddApplication(bridge);
    bridge->SetStartTime(Seconds(0.0));
    bridge->SetStopTime(Seconds(simTime));

    // Start real TCP threads
    std::thread acceptThread(TcpAcceptThread, curlPort);
    std::thread respThread(TcpResponseThread);

    NS_LOG_UNCOND("");
    NS_LOG_UNCOND("Simulation running for " << simTime << "s. Keys generated after ~2-3s.");
    NS_LOG_UNCOND("Press Ctrl+C to stop.");

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    g_running = false;
    g_respCV.notify_all();
    acceptThread.join();
    respThread.join();

    QLinkHelper.PrintGraphs();
    Simulator::Destroy();

    if (g_sslCtx) {
        SSL_CTX_free(g_sslCtx);
    }
    EVP_cleanup();
    return 0;
}
