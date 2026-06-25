// #include "ns3/core-module.h"
// #include "ns3/network-module.h"
// #include "ns3/internet-module.h"
// #include "ns3/point-to-point-module.h"
// #include "ns3/applications-module.h"
// #include "ns3/tap-bridge-module.h"

// using namespace ns3;

// NS_LOG_COMPONENT_DEFINE ("CurlTapBridgeExample");

// int main (int argc, char *argv[])
// {
//   CommandLine cmd (__FILE__);
//   cmd.Parse (argc, argv);

//   // 1. CRITICAL: Force ns-3 to synchronize with your real-world wall clock
//   GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

//   // 2. Create the network nodes
//   NodeContainer nodes;
//   nodes.Create (2); // Node 0 (Connected to Linux OS) <---> Node 1 (Simulated Server)

//   // 3. Connect them with a simulated point-to-point link
//   PointToPointHelper p2p;
//   p2p.SetDeviceAttribute ("DataRate", StringValue ("10Mbps"));
//   p2p.SetChannelAttribute ("Delay", StringValue ("50ms")); // Simulated delay

//   NetDeviceContainer devices;
//   devices.Add (p2p.Install (nodes.Get (0), nodes.Get (1)));

//   // 4. Install the Internet Stack on simulated nodes
//   InternetStackHelper stack;
//   stack.Install (nodes);

//   // 5. Assign IP addresses
//   Ipv4AddressHelper address;
//   address.SetBase ("10.1.1.0", "255.255.255.0");
//   Ipv4InterfaceContainer interfaces = address.Assign (devices);

//   NS_LOG_UNCOND ("Node 0 (Tap Boundary) IP: " << interfaces.GetAddress (0));
//   NS_LOG_UNCOND ("Node 1 (Simulated Server) IP: " << interfaces.GetAddress (1));

//   // 6. Install an ns-3 Packet Sink on Node 1 to catch the TCP traffic on Port 80
//   uint16_t port = 80; 
//   Address sinkLocalAddress (InetSocketAddress (Ipv4Address::GetAny (), port));
//   PacketSinkHelper packetSinkHelper ("ns3::TcpSocketFactory", sinkLocalAddress);
//   ApplicationContainer sinkApp = packetSinkHelper.Install (nodes.Get (1));
//   sinkApp.Start (Seconds (0.0));
//   sinkApp.Stop (Seconds (600.0));

//   // 7. Create the TapBridge to bridge your real OS to Node 0's device
//   TapBridgeHelper tapBridge;
//   tapBridge.SetAttribute ("Mode", StringValue ("UseLocal"));
//   tapBridge.SetAttribute ("DeviceName", StringValue ("tap0"));
//   // This explicitly bridges our real Linux tap0 to the ns-3 NetDevice on Node 0
//   tapBridge.Install (nodes.Get (0), devices.Get (0));

//   // 8. Run the real-time simulation
//   NS_LOG_UNCOND ("Simulation running. Press Ctrl+C to stop or wait for timeout.");
//   Simulator::Stop (Seconds (600.0));
//   Simulator::Run ();
//   Simulator::Destroy ();
//   return 0;
// }

