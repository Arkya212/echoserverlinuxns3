/*
 * Bridge between real TCP socket and NS-3 simulated network
 * 
 * Flow: curl → Real Socket → NS-3 Node Application → Response → Real Socket → curl
*/

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
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

NS_LOG_COMPONENT_DEFINE("CurlBridgedSocket");

// Request/Response queue to communicate between real socket and NS-3 simulation
struct HttpRequest {
    int clientFd;
    std::string path;
    double arrivalTime;
};

struct HttpResponse {
    int clientFd;
    std::string body;
};

std::queue<HttpRequest> g_requestQueue;
std::queue<HttpResponse> g_responseQueue;
std::mutex g_requestMutex;
std::mutex g_responseMutex;
std::condition_variable g_requestCV;
std::condition_variable g_responseCV;
std::atomic<bool> g_serverRunning(true);

// Custom NS-3 Application that processes HTTP requests
class BridgedHttpServer : public Application
{
public:
    BridgedHttpServer() : m_requestsProcessed(0) {}
    virtual ~BridgedHttpServer() {}

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("BridgedHttpServer")
            .SetParent<Application>()
            .SetGroupName("Applications")
            .AddConstructor<BridgedHttpServer>();
        return tid;
    }

private:
    virtual void StartApplication()
    {
        NS_LOG_INFO("BridgedHttpServer started on node " << GetNode()->GetId());
        // Schedule periodic check for new requests
        m_checkEvent = Simulator::Schedule(MilliSeconds(10), &BridgedHttpServer::CheckForRequests, this);
    }

    virtual void StopApplication()
    {
        Simulator::Cancel(m_checkEvent);
    }

    void CheckForRequests()
    {
        std::unique_lock<std::mutex> lock(g_requestMutex);
        
        while (!g_requestQueue.empty()) {
            HttpRequest req = g_requestQueue.front();
            g_requestQueue.pop();
            lock.unlock();

            NS_LOG_INFO("Node " << GetNode()->GetId() << " processing request: " << req.path 
                        << " (arrived at " << req.arrivalTime << "s, processing at " 
                        << Simulator::Now().GetSeconds() << "s)");

            double processingDelay = 0.05; // 50ms simulated processing time
            
            // Response
            std::ostringstream jsonBody;
            jsonBody << "{\n"
                     << "  \"keys\": [\n"
                     << "    {\"key_ID\": \"ns3-key: " << m_requestsProcessed << "\", "
                     << "\"key\": \"" << GenerateBase64Key() << "\"},\n"
                     << "    {\"key_ID\": \"ns3-key: " << (m_requestsProcessed + 1) << "\", "
                     << "\"key\": \"" << GenerateBase64Key() << "\"}\n"
                     << "  ],\n"
                     << "  \"source\": \"ns3-node-" << GetNode()->GetId() << "\",\n"
                     << "  \"node_ip\": \"" << GetNode()->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal() << "\",\n"
                     << "  \"request_arrival_time\": " << req.arrivalTime << ",\n"
                     << "  \"processing_time\": " << Simulator::Now().GetSeconds() << ",\n"
                     << "  \"simulated_delay\": " << processingDelay << "\n"
                     << "}\n";

            m_requestsProcessed += 2;

            // Schedule response after simulated processing delay
            Simulator::Schedule(Seconds(processingDelay), &BridgedHttpServer::SendResponse, 
                              this, req.clientFd, jsonBody.str());

            lock.lock();
        }

        // Schedule next check
        m_checkEvent = Simulator::Schedule(MilliSeconds(10), &BridgedHttpServer::CheckForRequests, this);
    }

    void SendResponse(int clientFd, std::string body)
    {
        NS_LOG_INFO("Node " << GetNode()->GetId() << " sending response at " 
                    << Simulator::Now().GetSeconds() << "s");

        std::lock_guard<std::mutex> lock(g_responseMutex);
        g_responseQueue.push({clientFd, body});
        g_responseCV.notify_one();
    }

    std::string GenerateBase64Key()
    {
        // Simple mock base64 key
        static const char* keys[] = {
            "YWJjZGVmZ2hpamtsbW5vcA==",
            "cXJzdHV2d3h5ejAxMjM0NQ==",
            "MTIzNDU2Nzg5MGFiY2RlZg==",
            "ZGVmZ2hpamtsbW5vcHFyc3Q="
        };
        return keys[m_requestsProcessed % 4];
    }

    EventId m_checkEvent;
    uint32_t m_requestsProcessed;
};

NS_OBJECT_ENSURE_REGISTERED(BridgedHttpServer);

// Real TCP socket server thread
void RealTcpServerThread(uint16_t port)
{
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        NS_LOG_ERROR("Failed to create socket");
        return;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        NS_LOG_ERROR("Bind failed on port " << port);
        close(serverFd);
        return;
    }

    if (listen(serverFd, 10) < 0) {
        NS_LOG_ERROR("Listen failed");
        close(serverFd);
        return;
    }

    NS_LOG_UNCOND("Real TCP server listening on http://0.0.0.0:" << port);
    NS_LOG_UNCOND("Requests will be processed by NS-3 simulated nodes");
    NS_LOG_UNCOND("");

    while (g_serverRunning) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverFd, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(serverFd + 1, &readfds, NULL, NULL, &timeout);
        if (activity <= 0) continue;

        struct sockaddr_in clientAddr;
        socklen_t addrlen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &addrlen);
        
        if (clientFd < 0) continue;

        // Read HTTP request
        char buffer[4096] = {0};
        ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
        
        if (bytesRead > 0) {
            // Parse request path
            std::string request(buffer);
            size_t pathStart = request.find(" ") + 1;
            size_t pathEnd = request.find(" ", pathStart);
            std::string path = request.substr(pathStart, pathEnd - pathStart);

            NS_LOG_INFO("Received HTTP request: " << path);

            // Forward to NS-3 simulation
            {
                std::lock_guard<std::mutex> lock(g_requestMutex);
                g_requestQueue.push({clientFd, path, Simulator::Now().GetSeconds()});
                g_requestCV.notify_one();
            }
        }
    }

    close(serverFd);
    NS_LOG_UNCOND("Real TCP server stopped");
}

// Response sender thread
void ResponseSenderThread()
{
    while (g_serverRunning) {
        std::unique_lock<std::mutex> lock(g_responseMutex);
        g_responseCV.wait_for(lock, std::chrono::milliseconds(100), 
                             []{ return !g_responseQueue.empty() || !g_serverRunning; });

        while (!g_responseQueue.empty()) {
            HttpResponse resp = g_responseQueue.front();
            g_responseQueue.pop();
            lock.unlock();

            // Send HTTP response
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Connection: close\r\n"
                     << "Content-Length: " << resp.body.length() << "\r\n"
                     << "\r\n"
                     << resp.body;

            std::string responseStr = response.str();
            write(resp.clientFd, responseStr.c_str(), responseStr.length());
            close(resp.clientFd);

            lock.lock();
        }
    }
}

int main(int argc, char* argv[])
{
    uint16_t serverPort = 8080;

    CommandLine cmd(__FILE__);
    cmd.AddValue("port", "TCP server port", serverPort);
    cmd.Parse(argc, argv);

    // Use real-time simulator for immediate response
    GlobalValue::Bind("SimulatorImplementationType", 
                      StringValue("ns3::RealtimeSimulatorImpl"));

    // Create NS-3 network topology
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("10ms"));

    NetDeviceContainer devices = p2p.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    NS_LOG_UNCOND("=== NS-3 Simulated Network ===");
    NS_LOG_UNCOND("Node 0 IP: " << interfaces.GetAddress(0));
    NS_LOG_UNCOND("Node 1 IP: " << interfaces.GetAddress(1) << " (HTTP processor)");
    NS_LOG_UNCOND("");

    // Install BridgedHttpServer on Node 1
    Ptr<BridgedHttpServer> serverApp = CreateObject<BridgedHttpServer>();
    nodes.Get(1)->AddApplication(serverApp);
    serverApp->SetStartTime(Seconds(0.0));
    serverApp->SetStopTime(Seconds(600.0));

    // Start real TCP server and response sender threads
    std::thread serverThread(RealTcpServerThread, serverPort);
    std::thread responseThread(ResponseSenderThread);

    NS_LOG_UNCOND("Simulation running. Requests flow: curl → socket → NS-3 node → socket → curl");
    NS_LOG_UNCOND("Press Ctrl+C to stop.");
    NS_LOG_UNCOND("");
    
    Simulator::Stop(Seconds(600.0));
    Simulator::Run();
    
    // Cleanup
    g_serverRunning = false;
    g_requestCV.notify_all();
    g_responseCV.notify_all();
    serverThread.join();
    responseThread.join();
    
    Simulator::Destroy();
    return 0;
}
