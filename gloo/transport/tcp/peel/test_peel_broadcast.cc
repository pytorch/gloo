/**
 * Test Peel multicast broadcast using Gloo
 * 
 * Usage:
 *   ./test_peel_broadcast <rank> <world_size> <redis_host> [redis_port] [mcast_group] [mcast_port]
 * 
 * Example (3 nodes):
 *   Node 0: ./test_peel_broadcast 0 3 192.168.1.101
 *   Node 1: ./test_peel_broadcast 1 3 192.168.1.101
 *   Node 2: ./test_peel_broadcast 2 3 192.168.1.101
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "gloo/rendezvous/redis_store.h"
#include "gloo/transport/tcp/device.h"
#include "gloo/transport/tcp/context.h"
#include "gloo/transport/tcp/peel/peel_context.h"

using Clock = std::chrono::steady_clock;

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog 
              << " <rank> <world_size> <redis_host> [redis_port] [mcast_group] [mcast_port]\n";
    std::cerr << "\nDefaults:\n";
    std::cerr << "  redis_port:  6379\n";
    std::cerr << "  mcast_group: 239.255.0.1\n";
    std::cerr << "  mcast_port:  5000\n";
    std::cerr << "\nExample:\n";
    std::cerr << "  " << prog << " 0 3 192.168.1.101\n";
    std::cerr << "  " << prog << " 1 3 192.168.1.101\n";
    std::cerr << "  " << prog << " 2 3 192.168.1.101\n";
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
    std::string mcastGroup = (argc > 5) ? argv[5] : "239.255.0.1";
    uint16_t mcastPort = (argc > 6) ? static_cast<uint16_t>(std::atoi(argv[6])) : 5000;

    std::cout << "[PEEL] Rank " << rank << "/" << worldSize 
              << " connecting to Redis at " << redisHost << ":" << redisPort << "\n";
    std::cout << "[PEEL] Rank " << rank << ": Multicast group " 
              << mcastGroup << ":" << mcastPort << "\n";

    // ===========================================
    // Setup Redis store for rendezvous
    // ===========================================
    gloo::rendezvous::RedisStore store(redisHost, redisPort);

    // ===========================================
    // Create TCP device and context
    // ===========================================
    gloo::transport::tcp::attr tcpAttr;
    // tcpAttr.iface = "eth0";  // Uncomment and set if needed
    auto tcpDevice = gloo::transport::tcp::CreateDevice(tcpAttr);

    std::cout << "[PEEL] Rank " << rank << ": TCP device created\n";

    // Create TCP context directly
    auto tcpContext = tcpDevice->createContext(rank, worldSize);
    auto* tcpCtx = dynamic_cast<gloo::transport::tcp::Context*>(tcpContext.get());

    if (!tcpCtx) {
        std::cerr << "[PEEL] Rank " << rank << ": Failed to get TCP context\n";
        return 1;
    }

    // Connect using store
    tcpCtx->createAndConnectAllPairs(
        std::make_shared<gloo::rendezvous::RedisStore>(redisHost, redisPort));

    std::cout << "[PEEL] Rank " << rank << ": TCP context connected\n";

    // ===========================================
    // Enable Peel multicast
    // ===========================================
    gloo::transport::tcp::peel::PeelContextConfig peelConfig;
    peelConfig.rank = rank;
    peelConfig.world_size = worldSize;
    peelConfig.redis_host = redisHost;
    peelConfig.redis_port = redisPort;
    peelConfig.mcast_group = mcastGroup;
    peelConfig.base_port = mcastPort;
    peelConfig.redis_prefix = "peel_test";

    std::cout << "[PEEL] Rank " << rank << ": Enabling Peel...\n";

    tcpCtx->enablePeel(peelConfig);

    if (!tcpCtx->isPeelReady()) {
        std::cerr << "[PEEL] Rank " << rank << ": Failed to initialize Peel\n";
        return 1;
    }

    std::cout << "[PEEL] Rank " << rank << ": Peel initialized and ready\n";

    // ===========================================
    // Prepare test data
    // ===========================================
    const size_t dataCount = 1024 * 1024;  // 1M elements = 4MB for uint32_t
    std::vector<uint32_t> data(dataCount);

    const int root = 0;

    if (rank == root) {
        // Root fills data with known pattern
        std::iota(data.begin(), data.end(), 0);  // 0, 1, 2, 3, ...
        std::cout << "[PEEL] Rank " << rank << ": Sender - broadcasting " 
                  << dataCount << " uint32s (" << (dataCount * sizeof(uint32_t)) << " bytes)\n";
    } else {
        // Receivers initialize to zero
        std::fill(data.begin(), data.end(), 0);
        std::cout << "[PEEL] Rank " << rank << ": Receiver - waiting for broadcast\n";
    }

    // ===========================================
    // Perform Peel broadcast and measure time
    // ===========================================
    auto startTime = Clock::now();

    bool ok = tcpCtx->peelBroadcast(root, data.data(), dataCount * sizeof(uint32_t));

    auto endTime = Clock::now();

    if (!ok) {
        std::cerr << "[PEEL] Rank " << rank << ": Broadcast FAILED\n";
        return 1;
    }

    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime).count();
    double durationMs = durationUs / 1000.0;
    double throughputMBps = (dataCount * sizeof(uint32_t)) / (durationUs / 1e6) / (1024 * 1024);

    std::cout << "[PEEL] Rank " << rank << ": Broadcast completed in " 
              << durationMs << " ms (" << throughputMBps << " MB/s)\n";

    // ===========================================
    // Verify data
    // ===========================================
    bool success = true;
    for (size_t i = 0; i < dataCount; ++i) {
        if (data[i] != static_cast<uint32_t>(i)) {
            std::cerr << "[PEEL] Rank " << rank << ": VERIFICATION FAILED at index " 
                      << i << " (expected " << i << ", got " << data[i] << ")\n";
            success = false;
            break;
        }
    }

    if (success) {
        std::cout << "[PEEL] Rank " << rank << ": SUCCESS - Data verified correctly\n";
    }

    // ===========================================
    // Barrier before exit
    // ===========================================
    std::string barrierKey = "peel_barrier_" + std::to_string(rank);
    store.set(barrierKey, std::vector<char>{'d', 'o', 'n', 'e'});
    
    for (int r = 0; r < worldSize; ++r) {
        std::string key = "peel_barrier_" + std::to_string(r);
        store.wait({key});
    }

    std::cout << "[PEEL] Rank " << rank << ": Exiting\n";
    return success ? 0 : 1;
}