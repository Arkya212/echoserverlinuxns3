/*
 * NS-3 simulation with a REAL TCP socket server
 * 
 * This creates a real POSIX TCP socket listening on localhost:8080
 * that you can query with curl while the simulation runs.
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
#include <cstring>
#include <sstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CurlRealSocketExample");

std::atomic<bool> g_serverRunning(true);

void RealTcpServerThread(uint16_t port)
{
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        NS_LOG_ERROR("Failed to create socket");
        return;
    }

    // Allow port reuse
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

    if (listen(serverFd, 3) < 0) {
        NS_LOG_ERROR("Listen failed");
        close(serverFd);
        return;
    }

    NS_LOG_UNCOND("Real TCP server listening on http://localhost:" << port);
    NS_LOG_UNCOND("Try: curl http://localhost:" << port << "/api/v1/keys/alice/enc_keys");

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
        read(clientFd, buffer, sizeof(buffer) - 1);

        // Simple HTTP response with mock QKD keys
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Connection: close\r\n\r\n"
                 << "{\n"
                 << "  \"keys\": [\n"
                 << "    {\"key_ID\": \"sim-key-001\", \"key\": \"YWJjZGVmZ2hpamtsbW5vcA==\"},\n"
                 << "    {\"key_ID\": \"sim-key-002\", \"key\": \"cXJzdHV2d3h5ejAxMjM0NQ==\"}\n"
                 << "  ],\n"
                 << "  \"source\": \"ns3-simulation\",\n"
                 << "  \"timestamp\": " << Simulator::Now().GetSeconds() << "\n"
                 << "}\n";

        std::string responseStr = response.str();
        write(clientFd, responseStr.c_str(), responseStr.length());
        close(clientFd);

        NS_LOG_INFO("Served HTTP request at simulation time " << Simulator::Now().GetSeconds() << "s");
    }

    close(serverFd);
    NS_LOG_UNCOND("Real TCP server stopped");
}

int main(int argc, char* argv[])
{
    uint16_t serverPort = 8080;

    CommandLine cmd(__FILE__);
    cmd.AddValue("port", "TCP server port", serverPort);
    cmd.Parse(argc, argv);

    GlobalValue::Bind("SimulatorImplementationType", 
                      StringValue("ns3::RealtimeSimulatorImpl"));

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

    NS_LOG_UNCOND("NS-3 simulated network:");
    NS_LOG_UNCOND("  Node 0 IP: " << interfaces.GetAddress(0));
    NS_LOG_UNCOND("  Node 1 IP: " << interfaces.GetAddress(1));
    NS_LOG_UNCOND("");

    // Start the real TCP server in a separate thread
    std::thread serverThread(RealTcpServerThread, serverPort);

    // Run simulation for 600 seconds (10 minutes)
    NS_LOG_UNCOND("Simulation running. Press Ctrl+C to stop.");
    NS_LOG_UNCOND("");
    
    Simulator::Stop(Seconds(600.0));
    Simulator::Run();
    
    // Stop the server thread
    g_serverRunning = false;
    serverThread.join();
    
    Simulator::Destroy();
    return 0;
}
