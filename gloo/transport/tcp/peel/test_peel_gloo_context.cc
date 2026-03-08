/**
 * Test program for Peel handshake via Gloo TCP Context.
 */

#include <gloo/transport/tcp/context.h>
#include <gloo/transport/tcp/device.h>
#include <gloo/transport/tcp/attr.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <thread>

// Helper: resolve interface name to IP address
static std::string getInterfaceIP(const std::string& iface_name) {
    if (iface_name.empty()) {
        return "";
    }
    
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "";
    }
    
    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (iface_name != ifa->ifa_name) continue;
        
        char ip_str[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str))) {
            result = ip_str;
            break;
        }
    }
    
    freeifaddrs(ifaddr);
    return result;
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --rank <R> --size <N> [options]\n"
              << "\n"
              << "Required:\n"
              << "  --rank <R>        This process's rank (0 = sender)\n"
              << "  --size <N>        Total number of processes\n"
              << "\n"
              << "Peel Options:\n"
              << "  --group <IP>      Multicast group (default: 239.255.0.1)\n"
              << "  --mcast-port <P>  Multicast port (default: 5000)\n"
              << "  --iface <NAME>    Interface NAME like eno16np0 (not IP!)\n"
              << "  --timeout <MS>    Handshake timeout (default: 30000)\n"
              << "  --rto <MS>        Retransmission timeout (default: 250)\n"
              << "\n"
              << "Example (2 processes):\n"
              << "  Terminal 1: " << prog << " --rank 1 --size 2 --iface eno16np0\n"
              << "  Terminal 2: " << prog << " --rank 0 --size 2 --iface eno16np0\n";
}

int main(int argc, char** argv) {
    int rank = -1;
    int size = -1;
    
    // Peel configuration
    std::string mcast_group = "239.255.0.1";
    uint16_t mcast_port = 5000;
    std::string iface_name;  // Interface NAME (e.g., eno16np0)
    int timeout_ms = 30000;
    int rto_ms = 250;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        auto need_arg = [&]() -> bool {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires an argument\n";
                return false;
            }
            return true;
        };

        if (arg == "--rank" && need_arg()) {
            rank = std::stoi(argv[++i]);
        } else if (arg == "--size" && need_arg()) {
            size = std::stoi(argv[++i]);
        } else if (arg == "--group" && need_arg()) {
            mcast_group = argv[++i];
        } else if (arg == "--mcast-port" && need_arg()) {
            mcast_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--iface" && need_arg()) {
            iface_name = argv[++i];
        } else if (arg == "--timeout" && need_arg()) {
            timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--rto" && need_arg()) {
            rto_ms = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // Validate arguments
    if (rank < 0 || size <= 0) {
        std::cerr << "Error: --rank and --size are required\n";
        usage(argv[0]);
        return 1;
    }

    if (rank >= size) {
        std::cerr << "Error: rank (" << rank << ") must be < size (" << size << ")\n";
        return 1;
    }

    // Resolve interface name to IP for Peel
    std::string iface_ip;
    if (!iface_name.empty()) {
        iface_ip = getInterfaceIP(iface_name);
        if (iface_ip.empty()) {
            std::cerr << "Error: Could not resolve interface '" << iface_name << "' to IP\n";
            return 1;
        }
    }

    bool is_sender = (rank == 0);
    int expected_receivers = size - 1;

    std::cerr << "============================================\n"
              << "  Peel + Gloo TCP Context Integration Test\n"
              << "============================================\n"
              << "Rank: " << rank << " / " << size << "\n"
              << "Role: " << (is_sender ? "SENDER (rank 0)" : "RECEIVER") << "\n"
              << "Multicast: " << mcast_group << ":" << mcast_port << "\n"
              << "Interface: " << iface_name << " (IP: " << iface_ip << ")\n"
              << "Timeout: " << timeout_ms << " ms\n"
              << "RTO: " << rto_ms << " ms\n"
              << "============================================\n\n";

    try {
        // ============================================================
        // Step 1: Create Gloo TCP Device (uses interface NAME)
        // ============================================================
        std::cerr << "[Step 1] Creating Gloo TCP Device...\n";
        
        ::gloo::transport::tcp::attr attr;
        if (!iface_name.empty()) {
            attr.iface = iface_name;  // Gloo wants interface NAME
        }
        
        auto device = ::gloo::transport::tcp::CreateDevice(attr);
        std::cerr << "  Device created: " << device->str() << "\n\n";

        // ============================================================
        // Step 2: Create Gloo TCP Context
        // ============================================================
        std::cerr << "[Step 2] Creating Gloo TCP Context (rank=" << rank 
                  << ", size=" << size << ")...\n";
        
        auto context = device->createContext(rank, size);
        
        // Cast to TCP context to access Peel methods
        auto* tcpContext = dynamic_cast<::gloo::transport::tcp::Context*>(context.get());
        if (!tcpContext) {
            std::cerr << "  ERROR: Failed to cast to TCP Context!\n";
            return 2;
        }
        std::cerr << "  TCP Context created successfully.\n\n";

        // ============================================================
        // Step 3: Enable Peel on the Context (uses interface IP)
        // ============================================================
        std::cerr << "[Step 3] Enabling Peel handshake on Context...\n";
        
        ::gloo::transport::tcp::peel::PeelConfig peelConfig;
        peelConfig.group = mcast_group;
        peelConfig.mcast_port = mcast_port;
        peelConfig.iface_ip = iface_ip;  // Peel wants IP address
        peelConfig.handshake_timeout_ms = timeout_ms;
        peelConfig.rto_ms = rto_ms;

        
        tcpContext->enablePeel(peelConfig);
        
        std::cerr << "  Peel enabled: isPeelEnabled() = " 
                  << (tcpContext->isPeelEnabled() ? "true" : "false") << "\n\n";

        // ============================================================
        // Step 4: Perform Peel Handshake
        // ============================================================
        if (!is_sender) {
            std::cerr << "[Step 4] RECEIVER: Waiting for sender's SYN...\n";
            std::cerr << "         (Start the sender process now if not started)\n\n";
        } else {
            std::cerr << "[Step 4] SENDER: Starting handshake with " 
                      << expected_receivers << " receiver(s)...\n";
            std::cerr << "         (Make sure all receivers are running!)\n\n";
            
            // Small delay to ensure receivers are ready
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        bool handshake_ok = tcpContext->performPeelHandshake(is_sender, expected_receivers);

        if (!handshake_ok) {
            std::cerr << "\n*** HANDSHAKE FAILED ***\n";
            std::cerr << "isPeelReady() = " << (tcpContext->isPeelReady() ? "true" : "false") << "\n";
            return 3;
        }

        // ============================================================
        // Step 5: Verify Results
        // ============================================================
        std::cerr << "\n============================================\n"
                  << "  *** HANDSHAKE SUCCESS ***\n"
                  << "============================================\n";

        std::cerr << "isPeelReady() = " << (tcpContext->isPeelReady() ? "true" : "false") << "\n";

        const auto* cohort = tcpContext->peelCohort();
        if (cohort) {
            std::cerr << "Cohort Info:\n"
                      << "  Socket FD: " << cohort->fd << "\n"
                      << "  Local Port: " << cohort->local_port << "\n"
                      << "  Peer Count: " << cohort->peers.size() << "\n"
                      << "  Peers:\n";

            for (size_t i = 0; i < cohort->peers.size(); ++i) {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cohort->peers[i].sin_addr, ip_str, sizeof(ip_str));
                std::cerr << "    [" << i << "] " << ip_str 
                          << ":" << ntohs(cohort->peers[i].sin_port) << "\n";
            }

            std::cerr << "  Multicast: ";
            {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cohort->mcast.sin_addr, ip_str, sizeof(ip_str));
                std::cerr << ip_str << ":" << ntohs(cohort->mcast.sin_port) << "\n";
            }
        } else {
            std::cerr << "WARNING: peelCohort() returned nullptr!\n";
        }

        std::cerr << "\n============================================\n"
                  << "  Rank " << rank << " TEST PASSED!\n"
                  << "============================================\n";

    } catch (const std::exception& ex) {
        std::cerr << "\n*** EXCEPTION: " << ex.what() << " ***\n";
        return 4;
    }

    return 0;
}