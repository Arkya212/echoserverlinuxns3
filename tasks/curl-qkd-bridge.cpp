/*
 * QKD Key Retrieval via curl - bridges real TCP socket to NS-3 KMS SBuffers
 *
 * Flow: curl -> Real Socket -> NS-3 KMS (SBuffer lookup) -> Real Socket -> curl
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

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CurlQKDBridge");

struct CurlRequest
 {
    int clientFd;
    std::string method;
    std::string path;
    std::string body;
};

struct CurlResponse 
{
    int clientFd;
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

    void StartApplication() override
    {
        m_pollEvent = Simulator::Schedule(MilliSeconds(50),
                                          &KmsBridgeApp::PollRequests, this);
    }

    void StopApplication() override { Simulator::Cancel(m_pollEvent); }

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
        m_pollEvent = Simulator::Schedule(MilliSeconds(50),
                                          &KmsBridgeApp::PollRequests, this);
    }

    void HandleRequest(const CurlRequest& req)
    {
        NS_LOG_INFO("Processing: " << req.method << " " << req.path
                    << " at sim-time " << Simulator::Now().GetSeconds() << "s");

        std::string respBody;
        int status = 200;

        // Determine which KMS based on path (/alice or /bob)
        Ptr<QKDKeyManagerSystemApplication> targetKms = g_kmsA;
        if (req.path.find("/bob/") != std::string::npos) {
            targetKms = g_kmsB;
        }

        if (req.path.find("/enc_keys") != std::string::npos) {
            respBody = DoGetKeys(req, targetKms);
        } else if (req.path.find("/dec_keys") != std::string::npos) {
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
        g_respQueue.push({req.clientFd, status, respBody});
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
    NS_LOG_UNCOND("=== QKD Key Server listening on port " << port << " ===");
    NS_LOG_UNCOND("Endpoints:");
    NS_LOG_UNCOND("  GET  http://<HOST>:" << port << "/api/v1/keys/any/status");
    NS_LOG_UNCOND("  GET  http://<HOST>:" << port << "/api/v1/keys/any/enc_keys");
    NS_LOG_UNCOND("  GET  http://<HOST>:" << port << "/api/v1/keys/any/enc_keys/number/3");
    NS_LOG_UNCOND("  POST http://<HOST>:" << port << "/api/v1/keys/any/dec_keys");
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

        // Read HTTP request (may need multiple reads for POST body)
        std::string raw;
        char buf[8192] = {};
        ssize_t nr = read(cfd, buf, sizeof(buf) - 1);
        if (nr <= 0) { close(cfd); continue; }
        raw.append(buf, nr);

        // Check if we have Content-Length and need to read more
        size_t clPos = raw.find("Content-Length: ");
        if (clPos != std::string::npos) {
            size_t clEnd = raw.find("\r\n", clPos);
            int contentLen = std::stoi(raw.substr(clPos + 16, clEnd - clPos - 16));
            size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                int bodyReceived = raw.size() - (headerEnd + 4);
                while (bodyReceived < contentLen) {
                    nr = read(cfd, buf, sizeof(buf) - 1);
                    if (nr <= 0) break;
                    raw.append(buf, nr);
                    bodyReceived += nr;
                }
            }
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
            g_reqQueue.push({cfd, method, path, body});
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
            ssize_t wr = write(r.clientFd, s.c_str(), s.size());
            (void)wr;
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
    */
    uint16_t curlPort = 43417;
    double simTime = 600.0;
    uint32_t ppKeySize = 512;
    std::string ppKeyRate = "100kbps";
    uint32_t ppPacketSize = 1000;
    std::string ppRate = "1Mbps";
    uint32_t numberOfKeyToFetchFromKMS = 3;

    CommandLine cmd(__FILE__);
    cmd.AddValue("port", "HTTP port for curl access", curlPort);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.Parse(argc, argv);

    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::RealtimeSimulatorImpl"));

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
    return 0;
}
