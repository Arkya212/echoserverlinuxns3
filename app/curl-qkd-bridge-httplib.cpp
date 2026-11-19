/*
 * QKD Key Retrieval via cpp-httplib (HTTPS)
 *
 * Refactored version of curl-qkd-bridge.cpp that replaces raw sockets,
 * manual HTTP parsing, request/response queues, and OpenSSL boilerplate
 * with cpp-httplib's SSLServer.
 *
 * Flow: curl -k https://HOST:8443/api/v1/keys/alice/status
 *       -> httplib::SSLServer -> KmsBridgeHandler -> NS-3 KMS SBuffers
 *
 * Generate certs:
 *   openssl req -x509 -newkey rsa:2048 -keyout key.pem \
 *     -out cert.pem -days 365 -nodes -subj '/CN=qkd-kms'
 */

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "kms-bridge-handler.h"

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

#include <cstdlib>
#include <thread>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CurlQKDBridgeHttplib");

static void SetupQkdTopology(NodeContainer& n,
                              Ptr<QKDKeyManagerSystemApplication>& kmsA,
                              Ptr<QKDKeyManagerSystemApplication>& kmsB,
                              double simTime,
                              uint32_t ppKeySize,
                              const std::string& ppKeyRate,
                              uint32_t ppPacketSize,
                              const std::string& ppRate,
                              uint32_t numberOfKeyToFetchFromKMS)
{
    n.Create(9);

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
    posAlloc->Add(Vector(0,   0,   0));   // node 0: app-A
    posAlloc->Add(Vector(200, 0,   0));   // node 1: relay
    posAlloc->Add(Vector(400, 0,   0));   // node 2: app-B
    posAlloc->Add(Vector(0,   100, 0));   // node 3: KMS-A
    posAlloc->Add(Vector(400, 100, 0));   // node 4: KMS-B
    posAlloc->Add(Vector(0,   200, 0));   // node 5: ctrl-A
    posAlloc->Add(Vector(400, 200, 0));   // node 6: ctrl-B
    posAlloc->Add(Vector(0,   50,  0));   // node 7: PP-slave
    posAlloc->Add(Vector(400, 50,  0));   // node 8: PP-master
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

    QKDAppHelper QAHelper;
    QKDLinkHelper QLinkHelper;

    Ptr<QKDControl> ctrlA = QLinkHelper.InstallQKDNController(n.Get(5));
    Ptr<QKDControl> ctrlB = QLinkHelper.InstallQKDNController(n.Get(6));
    QLinkHelper.ConfigureQBuffers({ctrlA, ctrlB}, 1024, 1800, 500000000, 512);

    QAHelper.InstallKeyManager(n.Get(3), i3i4.GetAddress(0), 80, ctrlA);
    QAHelper.InstallKeyManager(n.Get(4), i3i4.GetAddress(1), 80, ctrlB);

    kmsA = n.Get(3)->GetApplication(0)->GetObject<QKDKeyManagerSystemApplication>();
    kmsB = n.Get(4)->GetApplication(0)->GetObject<QKDKeyManagerSystemApplication>();

    NS_LOG_UNCOND("KMS-A on node 3, IP: " << i3i4.GetAddress(0));
    NS_LOG_UNCOND("KMS-B on node 4, IP: " << i3i4.GetAddress(1));

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

    // NOTE: QKDApp014 crypto apps are disabled so that keys accumulate
    // in the SBuffers and remain available for retrieval via the ETSI API.
    // Uncomment below to enable simulated encrypted communication (which
    // will consume keys from the same SBuffers the API queries).
    //
    // Config::SetDefault("ns3::QKDApp014::NumberOfKeyToFetchFromKMS",
    //                    UintegerValue(numberOfKeyToFetchFromKMS));
    // Config::SetDefault("ns3::QKDApp014::AuthenticationType", UintegerValue(1));
    // Config::SetDefault("ns3::QKDApp014::EncryptionType",     UintegerValue(2));
    // Config::SetDefault("ns3::QKDApp014::AESLifetime",        UintegerValue(10000));
    // Config::SetDefault("ns3::QKDApp014::UseCrypto",          UintegerValue(1));
    //
    // uint16_t commPort = 8081;
    // ApplicationContainer cryptoApps;
    // cryptoApps.Add(QAHelper.InstallQKDApplication(
    //     n.Get(0), n.Get(2),
    //     InetSocketAddress(i0i1.GetAddress(0), commPort),
    //     InetSocketAddress(i1i2.GetAddress(1), commPort),
    //     n.Get(5), n.Get(6),
    //     "tcp", 1000, DataRate("100kbps"), "etsi014"
    // ));
    // cryptoApps.Start(Seconds(5.0));
    // cryptoApps.Stop(Seconds(simTime - 5));

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    QLinkHelper.CreateTopologyGraph({ctrlA, ctrlB});
    QLinkHelper.PopulateRoutingTables();
    QLinkHelper.AddGraphs();
}

int main(int argc, char* argv[])
{
    uint16_t curlPort = 8443;
    double simTime = 600.0;
    uint32_t ppKeySize = 512;
    std::string ppKeyRate = "100kbps";
    uint32_t ppPacketSize = 1000;
    std::string ppRate = "1Mbps";
    uint32_t numberOfKeyToFetchFromKMS = 3;
    std::string certFile = "cert.pem";
    std::string keyFile  = "key.pem";

    // SAE and KME identifiers (env var fallback: MASTER_SAE_ID, SLAVE_SAE_ID, KME_ID_A, KME_ID_B)
    auto envOr = [](const char* name, const char* def) -> std::string {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : def;
    };
    std::string masterSae = envOr("MASTER_SAE_ID", "alice");
    std::string slaveSae  = envOr("SLAVE_SAE_ID",  "bob");
    std::string kmeIdA    = envOr("KME_ID_A",      "KME-A");
    std::string kmeIdB    = envOr("KME_ID_B",      "KME-B");

    CommandLine cmd(__FILE__);
    cmd.AddValue("port",      "HTTPS port for curl access",    curlPort);
    cmd.AddValue("simTime",   "Simulation time in seconds",    simTime);
    cmd.AddValue("cert",      "Path to TLS certificate (PEM)", certFile);
    cmd.AddValue("key",       "Path to TLS private key (PEM)", keyFile);
    cmd.AddValue("masterSae", "Master SAE ID",                  masterSae);
    cmd.AddValue("slaveSae",  "Slave/Peer SAE ID",              slaveSae);
    cmd.AddValue("kmeIdA",    "KME ID for master SAE",          kmeIdA);
    cmd.AddValue("kmeIdB",    "KME ID for slave SAE",           kmeIdB);
    cmd.Parse(argc, argv);

    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::RealtimeSimulatorImpl"));

    NodeContainer n;
    Ptr<QKDKeyManagerSystemApplication> kmsA, kmsB;
    SetupQkdTopology(n, kmsA, kmsB, simTime,
                     ppKeySize, ppKeyRate, ppPacketSize, ppRate,
                     numberOfKeyToFetchFromKMS);

    httplib::SSLServer svr(certFile.c_str(), keyFile.c_str());
    if (!svr.is_valid()) {
        NS_LOG_UNCOND("Failed to create HTTPS server. Generate certs first:");
        NS_LOG_UNCOND("  openssl req -x509 -newkey rsa:2048 -keyout key.pem \\");
        NS_LOG_UNCOND("    -out cert.pem -days 365 -nodes -subj '/CN=qkd-kms'");
        return 1;
    }

    // Register ETSI QKD 014 routes (configurable dual KMS setup)
    KmsBridgeHandler handler;
    handler.AddKmeEndpoint({kmeIdA, masterSae, slaveSae, kmeIdB, kmsA});
    handler.AddKmeEndpoint({kmeIdB, slaveSae, masterSae, kmeIdA, kmsB});
    handler.RegisterRoutes(svr);

    NS_LOG_UNCOND("Configured SAE IDs: master=" << masterSae << ", slave=" << slaveSae);
    NS_LOG_UNCOND("Configured KME IDs: " << kmeIdA << " (master), " << kmeIdB << " (slave)");

    // Run httplib in a background thread
    std::thread serverThread([&svr, curlPort]() {
        NS_LOG_UNCOND("");
        NS_LOG_UNCOND("=== QKD Key Server (HTTPS via cpp-httplib) on port "
                      << curlPort << " ===");
        NS_LOG_UNCOND("ETSI QKD 014 Endpoints:");
        NS_LOG_UNCOND("  GET  https://<HOST>:" << curlPort
                      << "/api/v1/keys/{master_SAE_ID}/status");
        NS_LOG_UNCOND("  GET  https://<HOST>:" << curlPort
                      << "/api/v1/keys/{slave_SAE_ID}/enc_keys?number=N&size=S");
        NS_LOG_UNCOND("  POST https://<HOST>:" << curlPort
                      << "/api/v1/keys/{master_SAE_ID}/dec_keys");
        NS_LOG_UNCOND("");
        NS_LOG_UNCOND("Usage:  curl -k https://localhost:" << curlPort
                      << "/api/v1/keys/{SAE_ID}/status");
        NS_LOG_UNCOND("        curl -k https://localhost:" << curlPort
                      << "/api/v1/keys/{SAE_ID}/enc_keys?number=3&size=256");
        NS_LOG_UNCOND("");

        NS_LOG_UNCOND("Attempting to Start the Server: ");
        bool success = svr.listen("0.0.0.0", curlPort);
        if (!success) {
            NS_LOG_UNCOND("Failed to start the server");
        }
    });

    NS_LOG_UNCOND("Simulation running for " << simTime
                  << "s. Keys generated after ~2-3s.");
    NS_LOG_UNCOND("Press Ctrl+C to stop.");

    // Simulator::Stop(Seconds(simTime)); 
    Simulator::Run();

    svr.stop();
    serverThread.join();

    Simulator::Destroy();
    return 0;
}
