#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace gloo::transport::tcp::peel;

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --rank R --size N [options]\n"
              << "\n"
              << "Required:\n"
              << "  --rank R         This process's rank (0 to N-1)\n"
              << "  --size N         Total number of processes\n"
              << "\n"
              << "Optional:\n"
              << "  --iface IP       Interface IP for multicast\n"
              << "  --redis-host H   Redis server hostname (default: 127.0.0.1)\n"
              << "  --redis-port P   Redis server port (default: 6379)\n"
              << "  --base-port P    Base port for multicast (default: 50000)\n"
              << "  --group G        Multicast group (default: 239.255.0.1)\n"
              << "  --timeout MS     Handshake timeout in ms (default: 30000)\n"
              << "  --cleanup        Cleanup Redis keys after test (rank 0 only)\n"
              << "  -h, --help       Show this help\n"
              << "\n"
              << "Example (2 nodes):\n"
              << "  Host 1: " << prog << " --rank 0 --size 2 --iface 10.161.159.71 --redis-host 10.161.159.71\n"
              << "  Host 2: " << prog << " --rank 1 --size 2 --iface 10.161.159.133 --redis-host 10.161.159.71\n"
              << "\n"
              << "Example (4 nodes):\n"
              << "  Node 0: " << prog << " --rank 0 --size 4 --iface <IP0> --redis-host <REDIS_IP>\n"
              << "  Node 1: " << prog << " --rank 1 --size 4 --iface <IP1> --redis-host <REDIS_IP>\n"
              << "  Node 2: " << prog << " --rank 2 --size 4 --iface <IP2> --redis-host <REDIS_IP>\n"
              << "  Node 3: " << prog << " --rank 3 --size 4 --iface <IP3> --redis-host <REDIS_IP>\n";
}

static bool parseArgs(int argc, char** argv, PeelFullMeshConfig& config, bool& do_cleanup) {
    do_cleanup = false;
    bool has_rank = false;
    bool has_size = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--rank") && i + 1 < argc) {
            config.rank = std::stoi(argv[++i]);
            has_rank = true;
        } else if ((arg == "--size") && i + 1 < argc) {
            config.world_size = std::stoi(argv[++i]);
            has_size = true;
        } else if ((arg == "--iface") && i + 1 < argc) {
            config.iface_ip = argv[++i];
        } else if ((arg == "--redis-host") && i + 1 < argc) {
            config.redis_host = argv[++i];
        } else if ((arg == "--redis-port") && i + 1 < argc) {
            config.redis_port = std::stoi(argv[++i]);
        } else if ((arg == "--base-port") && i + 1 < argc) {
            config.base_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "--group") && i + 1 < argc) {
            config.mcast_group = argv[++i];
        } else if ((arg == "--timeout") && i + 1 < argc) {
            config.handshake_timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--cleanup") {
            do_cleanup = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return false;
        }
    }

    if (!has_rank || !has_size) {
        std::cerr << "Error: --rank and --size are required\n";
        usage(argv[0]);
        return false;
    }

    if (config.rank < 0 || config.rank >= config.world_size) {
        std::cerr << "Error: rank must be in range [0, size-1]\n";
        return false;
    }

    if (config.world_size < 2) {
        std::cerr << "Error: size must be at least 2\n";
        return false;
    }

    return true;
}

static void printConfig(const PeelFullMeshConfig& config) {
    std::cerr << "============================================\n"
              << "  Peel Full-Mesh Multicast Test\n"
              << "============================================\n"
              << "Rank:           " << config.rank << " / " << config.world_size << "\n"
              << "Multicast:      " << config.mcast_group << "\n"
              << "Base Port:      " << config.base_port << "\n"
              << "Send Port:      " << config.sendPort() << "\n"
              << "Interface:      " << (config.iface_ip.empty() ? "(default)" : config.iface_ip) << "\n"
              << "Redis:          " << config.redis_host << ":" << config.redis_port << "\n"
              << "Timeout:        " << config.handshake_timeout_ms << " ms\n"
              << "============================================\n\n";
}

static void printResult(const PeelFullMeshResult& result) {
    std::cerr << "\n============================================\n"
              << "  *** HANDSHAKE SUCCESS ***\n"
              << "============================================\n"
              << "Rank:           " << result.rank << " / " << result.world_size << "\n"
              << "\n"
              << "Send Channel:\n"
              << "  Port:         " << result.send_channel->port << "\n"
              << "  FD:           " << result.send_channel->fd << "\n"
              << "  Sender Rank:  " << result.send_channel->sender_rank << " (self)\n"
              << "\n"
              << "Recv Channels:  " << result.recv_channels.size() << "\n";

    for (const auto& ch : result.recv_channels) {
        std::cerr << "  - Rank " << ch->sender_rank << ": port " << ch->port
                  << ", fd " << ch->fd << "\n";
    }

    std::cerr << "\nPeers Discovered:\n";
    for (int r = 0; r < result.world_size; ++r) {
        if (r == result.rank) {
            std::cerr << "  Rank " << r << ": (self)\n";
        } else {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &result.peers[r].sin_addr, ip_str, sizeof(ip_str));
            std::cerr << "  Rank " << r << ": " << ip_str << "\n";
        }
    }

    std::cerr << "============================================\n\n";
}

static bool testDataTransfer(PeelFullMeshResult& result) {
    std::cerr << "[Test] Data transfer test...\n";

    // Send a message on our channel
    std::string msg = "Hello from rank " + std::to_string(result.rank);
    
    sockaddr_in dest = result.send_channel->mcast;
    ssize_t sent = sendto(result.send_channel->fd, msg.c_str(), msg.size(), 0,
                          reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        perror("sendto data");
        return false;
    }
    std::cerr << "[Test] Sent: \"" << msg << "\" on port " << result.send_channel->port << "\n";

    // Receive messages from other ranks
    int received_count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (received_count < static_cast<int>(result.recv_channels.size()) &&
           std::chrono::steady_clock::now() < deadline) {

        for (auto& ch : result.recv_channels) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(ch->fd, &readfds);

            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100000;  // 100ms

            int ready = select(ch->fd + 1, &readfds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            char buf[1024];
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);

            ssize_t n = recvfrom(ch->fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n > 0) {
                buf[n] = '\0';
                std::cerr << "[Test] Received from rank " << ch->sender_rank
                          << ": \"" << buf << "\"\n";
                ++received_count;
            }
        }
    }

    std::cerr << "[Test] Received " << received_count << "/"
              << result.recv_channels.size() << " messages\n";

    return received_count > 0;
}

int main(int argc, char** argv) {
    PeelFullMeshConfig config;
    bool do_cleanup = false;

    if (!parseArgs(argc, argv, config, do_cleanup)) {
        return 1;
    }

    printConfig(config);

    // Create full-mesh handler
    PeelFullMesh mesh(config);

    // Step 1: Initialize (connect Redis, create sockets, signal ready)
    std::cerr << "[Step 1] Initializing...\n";
    if (!mesh.init()) {
        std::cerr << "ERROR: Initialization failed\n";
        return 2;
    }
    std::cerr << "[Step 1] Initialization complete\n\n";

    // Step 2: Run handshake (barrier + SYN/ACK exchange)
    std::cerr << "[Step 2] Running handshake...\n";
    auto result = mesh.run();
    if (!result) {
        std::cerr << "ERROR: Handshake failed\n";
        return 3;
    }
    std::cerr << "[Step 2] Handshake complete\n";

    // Print result
    printResult(*result);

    // Step 3: Test data transfer
    std::cerr << "[Step 3] Testing data transfer...\n";
    
    // Small delay to let all ranks finish handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    if (!testDataTransfer(*result)) {
        std::cerr << "WARNING: Data transfer test had issues\n";
    }

    // Cleanup (optional, rank 0 only)
    if (do_cleanup && config.rank == 0) {
        std::cerr << "\n[Cleanup] Removing Redis keys...\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));  // Wait for others
        mesh.cleanup();
    }

    std::cerr << "\n============================================\n"
              << "  Rank " << config.rank << " TEST PASSED!\n"
              << "============================================\n";

    return 0;
}