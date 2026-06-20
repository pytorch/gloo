/**
 * test_peel_allgather.cc — end-to-end sanity test for Peel multicast allgather.
 *
 * What this test does (in order):
 *   1. Parses command-line args and prints config.
 *   2. Runs PeelDiscovery: each rank publishes its interface IP via Redis so
 *      every rank ends up with a complete rank→IP map.
 *   3. Creates N PeelContext objects (one per sender rank), each rooted at a
 *      different rank and with a non-overlapping port range.
 *   4. Each rank fills its own send buffer with a recognizable pattern:
 *        buf[r][i] = r * dataCount + i
 *      All other buffers are zeroed (will be overwritten by broadcast).
 *   5. Constructs a PeelAllgather and calls run() — sequential or parallel
 *      depending on the --parallel flag.
 *   6. Measures total wall time and computes aggregate throughput.
 *   7. All ranks verify every buffer matches the expected pattern.
 *
 * NOTE: This test uses PeelContext directly — no TCP context is involved.
 *       PeelDiscovery is the only rendezvous step; it uses Redis directly.
 *
 * Usage:
 *   ./test_peel_allgather <rank> <world_size> <redis_host>
 *                         [redis_port] [iface] [mcast_group] [base_port]
 *                         [topology_file] [--parallel]
 *
 * Arguments:
 *   rank           — this process's rank (0-based)
 *   world_size     — total number of ranks
 *   redis_host     — Redis server IP for rendezvous (e.g. 10.0.0.1)
 *   redis_port     — Redis port (default: 6379)
 *   iface          — network interface for AF_PACKET raw socket (e.g. eth0)
 *                    REQUIRED
 *   mcast_group    — IPv4 multicast group (default: 239.255.0.1)
 *   base_port      — base UDP port; formula: base + sender*W*W + subtree*W
 *                    (default: 50000)
 *   topology_file  — path to adjacency topology file; omit for flat mode
 *   --parallel     — run all N broadcasts concurrently (default: sequential)
 *
 * Port isolation guarantee:
 *   Each sender r occupies ports [base + r*W*W, base + r*W*W + W*max_subtrees).
 *   With base=50000 and W=4: sender 0 → 50000-50015, sender 1 → 50016-50031, etc.
 *   All N contexts are open simultaneously — they never share a port.
 *
 * Examples:
 *   Sequential (default):
 *     ./test_peel_allgather 0 4 10.0.0.1 6379 eth0
 *
 *   Parallel:
 *     ./test_peel_allgather 0 4 10.0.0.1 6379 eth0 239.255.0.1 50000 topo.txt --parallel
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "gloo/transport/peel/peel_allgather.h"
#include "gloo/transport/peel/peel_context.h"
#include "gloo/transport/peel/peel_discovery.h"

using Clock = std::chrono::steady_clock;

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <rank> <world_size> <redis_host>"
                 " [redis_port] [iface] [mcast_group] [base_port] [topology_file] [--parallel]\n"
              << "\nDefaults:\n"
              << "  redis_port:    6379\n"
              << "  iface:         (required)\n"
              << "  mcast_group:   239.255.0.1\n"
              << "  base_port:     50000\n"
              << "  topology_file: (none — flat single-transport mode)\n"
              << "  --parallel:    off (sequential by default)\n"
              << "\nExamples:\n"
              << "  " << prog << " 0 4 10.0.0.1 6379 eth0\n"
              << "  " << prog << " 0 4 10.0.0.1 6379 eth0 239.255.0.1 50000 topo.txt --parallel\n";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    // =========================================================================
    // Step 1: Parse arguments
    // =========================================================================
    int         rank         = std::atoi(argv[1]);
    int         worldSize    = std::atoi(argv[2]);
    std::string redisHost    = argv[3];
    int         redisPort    = (argc > 4) ? std::atoi(argv[4]) : 6379;
    std::string iface        = (argc > 5) ? argv[5] : "";
    std::string mcastGroup   = (argc > 6) ? argv[6] : "239.255.0.1";
    uint16_t    basePort     = (argc > 7) ? static_cast<uint16_t>(std::atoi(argv[7])) : 50000;
    std::string topologyFile = "";
    bool        parallel     = false;

    // Scan remaining args for topology file and --parallel flag
    for (int i = 8; i < argc; ++i) {
        if (std::strcmp(argv[i], "--parallel") == 0) {
            parallel = true;
        } else {
            topologyFile = argv[i];
        }
    }

    auto modeStr = parallel ? "PARALLEL" : "SEQUENTIAL";

    std::cout << "\n[PEEL-AG] ====== STEP 1: Config ======\n"
              << "[PEEL-AG] Rank " << rank << "/" << worldSize
              << "  Redis=" << redisHost << ":" << redisPort << "\n"
              << "[PEEL-AG] iface=" << (iface.empty() ? "(not set)" : iface)
              << "  mcast=" << mcastGroup
              << "  base_port=" << basePort << "\n"
              << "[PEEL-AG] Topology: "
              << (topologyFile.empty() ? "FLAT (no file)" : topologyFile) << "\n"
              << "[PEEL-AG] Mode: " << modeStr << "\n";

    if (iface.empty()) {
        std::cerr << "[PEEL-AG] ERROR — iface argument is required for AF_PACKET\n";
        return 1;
    }

    // =========================================================================
    // Step 2: PeelDiscovery — build rank→IP map via Redis
    // =========================================================================
    std::cout << "\n[PEEL-AG] ====== STEP 2: PeelDiscovery ======\n";

    const std::string discoveryPrefix = "peel_allgather_test";

    gloo::transport::peel::PeelDiscoveryConfig discCfg;
    discCfg.rank         = rank;
    discCfg.world_size   = worldSize;
    discCfg.iface_name   = iface;
    discCfg.redis_host   = redisHost;
    discCfg.redis_port   = redisPort;
    discCfg.redis_prefix = discoveryPrefix;

    gloo::transport::peel::PeelDiscovery discovery(discCfg);
    if (!discovery.run()) {
        std::cerr << "[PEEL-AG] Rank " << rank << ": ERROR — discovery failed\n";
        return 1;
    }

    std::unordered_map<int, std::string> peerIps;
    for (int r = 0; r < worldSize; ++r)
        peerIps[r] = discovery.getIp(r);

    std::cout << "[PEEL-AG] Rank " << rank << ": Discovery complete — rank→IP map:\n";
    for (int r = 0; r < worldSize; ++r)
        std::cout << "[PEEL-AG]   rank " << r << " -> " << peerIps[r] << "\n";

    // =========================================================================
    // Step 3: Create N PeelContext objects — one per sender rank
    // =========================================================================
    std::cout << "\n[PEEL-AG] ====== STEP 3: Initializing " << worldSize
              << " PeelContext(s) ======\n";

    std::vector<std::unique_ptr<gloo::transport::peel::PeelContext>> ctxOwners;
    std::vector<gloo::transport::peel::PeelContext*>                 ctxPtrs;
    ctxOwners.reserve(worldSize);
    ctxPtrs.reserve(worldSize);

    for (int r = 0; r < worldSize; ++r) {
        gloo::transport::peel::PeelContextConfig cfg;
        cfg.rank          = rank;
        cfg.world_size    = worldSize;
        cfg.sender_rank   = r;
        cfg.peer_ips      = peerIps;
        cfg.base_port     = basePort;
        cfg.iface_name    = iface;
        cfg.mcast_group   = mcastGroup;
        cfg.topology_file = topologyFile;

        auto ctx = std::make_unique<gloo::transport::peel::PeelContext>(cfg);
        if (!ctx->init()) {
            std::cerr << "[PEEL-AG] Rank " << rank
                      << ": ERROR — PeelContext init failed for sender_rank=" << r << "\n";
            return 1;
        }
        if (!ctx->isReady()) {
            std::cerr << "[PEEL-AG] Rank " << rank
                      << ": ERROR — PeelContext not ready for sender_rank=" << r << "\n";
            return 1;
        }

        std::cout << "[PEEL-AG] Rank " << rank
                  << ": context[sender=" << r << "] ready ✓\n";
        ctxPtrs.push_back(ctx.get());
        ctxOwners.push_back(std::move(ctx));
    }

    // =========================================================================
    // Step 4: Prepare data buffers
    // =========================================================================
    std::cout << "\n[PEEL-AG] ====== STEP 4: Prepare data ======\n";

    const size_t dataCount = 256 * 1024;             // 256 K uint32 = 1 MB
    const size_t dataBytes = dataCount * sizeof(uint32_t);

    std::vector<std::vector<uint32_t>> bufs(
        worldSize, std::vector<uint32_t>(dataCount, 0));

    for (size_t i = 0; i < dataCount; ++i)
        bufs[rank][i] = static_cast<uint32_t>(rank * dataCount + i);

    // Build void* view for PeelAllgather::run()
    std::vector<void*> bufPtrs(worldSize);
    for (int r = 0; r < worldSize; ++r)
        bufPtrs[r] = bufs[r].data();

    std::cout << "[PEEL-AG] Rank " << rank << ": buffer[" << rank
              << "] filled with pattern (rank*" << dataCount << " + i).  "
              << "All other buffers zeroed.\n"
              << "[PEEL-AG] Buffer size per rank: "
              << dataBytes / 1024 << " KB  |  total allgather: "
              << (dataBytes * worldSize) / (1024 * 1024) << " MB\n";

    // =========================================================================
    // Step 5: Allgather via PeelAllgather
    // =========================================================================
    std::cout << "\n[PEEL-AG] ====== STEP 5: Allgather (" << worldSize
              << " broadcasts, " << modeStr << ") ======\n";

    auto mode = parallel
                    ? gloo::transport::peel::PeelAllgatherMode::Parallel
                    : gloo::transport::peel::PeelAllgatherMode::Sequential;

    gloo::transport::peel::PeelAllgather allgather(ctxPtrs, mode);

    auto t0 = Clock::now();
    bool ok = allgather.run(bufPtrs, dataBytes);
    auto t1 = Clock::now();

    if (!ok) {
        std::cerr << "[PEEL-AG] Rank " << rank << ": ERROR — allgather failed\n";
        return 1;
    }

    // =========================================================================
    // Step 6: Timing summary
    // =========================================================================
    auto   totalUs    = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double totalMs    = totalUs / 1000.0;
    double totalBytes = static_cast<double>(dataBytes) * worldSize;
    double throughput = totalBytes / (totalUs / 1e6) / (1024.0 * 1024.0);

    std::cout << "\n[PEEL-AG] ====== STEP 6: Timing ======\n"
              << "[PEEL-AG] Rank " << rank << ": allgather completed in "
              << totalMs << " ms  (aggregate throughput: "
              << throughput << " MB/s over " << worldSize << " broadcasts)\n";

    // =========================================================================
    // Step 7: Verify
    // =========================================================================
    std::cout << "\n[PEEL-AG] ====== STEP 7: Verify ======\n";

    bool allOk = true;
    for (int r = 0; r < worldSize; ++r) {
        bool bufOk = true;
        for (size_t i = 0; i < dataCount; ++i) {
            uint32_t expected = static_cast<uint32_t>(r * dataCount + i);
            if (bufs[r][i] != expected) {
                std::cerr << "[PEEL-AG] Rank " << rank
                          << ": FAILED — buf[sender=" << r << "][" << i << "] = "
                          << bufs[r][i] << ", expected " << expected
                          << "  (first mismatch)\n";
                bufOk = false;
                allOk = false;
                break;
            }
        }
        if (bufOk)
            std::cout << "[PEEL-AG] Rank " << rank
                      << ": buf[sender=" << r << "] — OK ✓\n";
    }

    if (allOk)
        std::cout << "\n[PEEL-AG] Rank " << rank
                  << ": SUCCESS — all " << worldSize << " buffers verified ✓\n\n";
    else
        std::cerr << "\n[PEEL-AG] Rank " << rank << ": FAILED\n\n";

    return allOk ? 0 : 1;
}
