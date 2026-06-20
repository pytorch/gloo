/**
 * Test TCP point-to-point broadcast using Gloo
 * 
 * Usage:
 *   ./test_tcp_broadcast <rank> <world_size> <redis_host> [redis_port] [iface]
 * 
 * Example (3 nodes):
 *   Node 0: ./test_tcp_broadcast 0 3 10.161.159.133 6379 eth0
 *   Node 1: ./test_tcp_broadcast 1 3 10.161.159.133 6379 eth0
 *   Node 2: ./test_tcp_broadcast 2 3 10.161.159.133 6379 eth0
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "gloo/broadcast.h"
#include "gloo/context.h"
#include "gloo/rendezvous/context.h"
#include "gloo/rendezvous/redis_store.h"
#include "gloo/transport/tcp/device.h"

using Clock = std::chrono::steady_clock;

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " <rank> <world_size> <redis_host> [redis_port] [iface]\n";
    std::cerr << "\nDefaults:\n";
    std::cerr << "  redis_port: 6379\n";
    std::cerr << "  iface:      (auto-detect)\n";
    std::cerr << "\nExample:\n";
    std::cerr << "  " << prog << " 0 3 10.161.159.133 6379 eth0\n";
    std::cerr << "  " << prog << " 1 3 10.161.159.133 6379 eth0\n";
    std::cerr << "  " << prog << " 2 3 10.161.159.133 6379 eth0\n";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    int rank = std::atoi(argv[1]);
    int worldSize = std::atoi(argv[2]);
    std::string redisHost = argv[3];
    int redisPort = (argc > 4) ? std::atoi(argv[4]) : 6379;
    std::string iface = (argc > 5) ? argv[5] : "";

    std::cout << "[TCP] Rank " << rank << "/" << worldSize 
              << " connecting to Redis at " << redisHost << ":" << redisPort << "\n";
    if (!iface.empty()) {
        std::cout << "[TCP] Rank " << rank << ": Using interface " << iface << "\n";
    }

    // ===========================================
    // Setup Redis store for rendezvous
    // ===========================================
    auto store = std::make_shared<gloo::rendezvous::RedisStore>(redisHost, redisPort);

    // ===========================================
    // Create TCP device with interface
    // ===========================================
    gloo::transport::tcp::attr tcpAttr;
    if (!iface.empty()) {
        tcpAttr.iface = iface;
    }
    auto tcpDevice = gloo::transport::tcp::CreateDevice(tcpAttr);

    std::cout << "[TCP] Rank " << rank << ": TCP device created\n";

    // ===========================================
    // Create context and connect full mesh
    // ===========================================
    auto context = std::make_shared<gloo::rendezvous::Context>(rank, worldSize);
    context->connectFullMesh(store, tcpDevice);

    std::cout << "[TCP] Rank " << rank << ": Full mesh connected\n";

    // ===========================================
    // Prepare test data
    // ===========================================
    const size_t dataCount = 1024 * 1024;  // 1M elements = 4MB for uint32_t
    std::vector<uint32_t> data(dataCount);

    const int root = 0;

    if (rank == root) {
        // Root fills data with known pattern
        std::iota(data.begin(), data.end(), 0);  // 0, 1, 2, 3, ...
        std::cout << "[TCP] Rank " << rank << ": Sender - broadcasting " 
                  << dataCount << " uint32s (" << (dataCount * sizeof(uint32_t)) << " bytes)\n";
    } else {
        // Receivers initialize to zero
        std::fill(data.begin(), data.end(), 0);
        std::cout << "[TCP] Rank " << rank << ": Receiver - waiting for broadcast\n";
    }

    // ===========================================
    // Perform broadcast and measure time
    // ===========================================
    auto startTime = Clock::now();

    // Use Gloo's TCP broadcast
    gloo::BroadcastOptions opts(context);
    opts.setRoot(root);
    opts.setOutput(data.data(), dataCount);
    gloo::broadcast(opts);

    auto endTime = Clock::now();
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime).count();
    double durationMs = durationUs / 1000.0;
    double throughputMBps = (dataCount * sizeof(uint32_t)) / (durationUs / 1e6) / (1024 * 1024);

    std::cout << "[TCP] Rank " << rank << ": Broadcast completed in " 
              << durationMs << " ms (" << throughputMBps << " MB/s)\n";

    // ===========================================
    // Verify data
    // ===========================================
    bool success = true;
    for (size_t i = 0; i < dataCount; ++i) {
        if (data[i] != static_cast<uint32_t>(i)) {
            std::cerr << "[TCP] Rank " << rank << ": VERIFICATION FAILED at index " 
                      << i << " (expected " << i << ", got " << data[i] << ")\n";
            success = false;
            break;
        }
    }

    if (success) {
        std::cout << "[TCP] Rank " << rank << ": SUCCESS - Data verified correctly\n";
    }

    // ===========================================
    // Barrier before exit
    // ===========================================
    std::string barrierKey = "tcp_barrier_" + std::to_string(rank);
    store->set(barrierKey, std::vector<char>{'d', 'o', 'n', 'e'});
    
    for (int r = 0; r < worldSize; ++r) {
        std::string key = "tcp_barrier_" + std::to_string(r);
        store->wait({key});
    }

    std::cout << "[TCP] Rank " << rank << ": Exiting\n";
    return success ? 0 : 1;
}