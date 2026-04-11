/**
 * test_peel_broadcast.cc — end-to-end sanity test for Peel multicast broadcast.
 *
 * What this test does (in order):
 *   1. Parses command-line args and prints config.
 *   2. Sets up a Redis-backed Gloo rendezvous store (shared coordination point).
 *   3. Creates a TCP device and Context, connects all rank pairs (standard Gloo).
 *   4. Runs PeelDiscovery: each rank publishes its interface IP via Redis so that
 *      every rank ends up with a complete rank→IP map.
 *   5. Builds a PeelContextConfig (with topology file + peer IPs if given) and
 *      calls tcpCtx->enablePeel(), which:
 *        a. If topology_file is set: builds PeelTree (MST + CIDR rule MAC per
 *           subtree), creates one PeelTransport per subtree, starts one worker
 *           thread per transport, runs PeelFullMesh handshake on each.
 *        b. If no topology_file: creates a single flat PeelTransport (all ranks,
 *           no CIDR rule), runs one handshake.
 *   6. Rank 0 fills a 4 MB buffer (0, 1, 2, ...), all others zero it.
 *   7. Calls tcpCtx->peelBroadcast(root=0, ...) — this fans out in parallel
 *      across all subtree worker threads (max time = slowest subtree).
 *   8. Measures elapsed time and computes throughput.
 *   9. All ranks verify the received data matches the expected sequence.
 *
 * Usage:
 *   ./test_peel_broadcast <rank> <world_size> <redis_host>
 *                         [redis_port] [iface] [mcast_group] [mcast_port]
 *                         [topology_file]
 *
 * Arguments:
 *   rank           — this process's rank (0-based)
 *   world_size     — total number of ranks
 *   redis_host     — Redis server IP for rendezvous (e.g. 10.0.0.1)
 *   redis_port     — Redis port (default: 6379)
 *   iface          — network interface for AF_PACKET raw socket (e.g. eth0, vmbr0)
 *                    REQUIRED — AF_PACKET needs an explicit interface name
 *   mcast_group    — IPv4 multicast group (default: 239.255.0.1)
 *   mcast_port     — base UDP port; each rank binds base_port+rank per subtree
 *                    (default: 5000)
 *   topology_file  — path to adjacency topology file for PeelTree; omit for flat
 *                    single-transport mode (no CIDR tree optimisation)
 *
 * Topology file format (one line per node):
 *   <node_id>  <neighbour_0>  <neighbour_1>  ...
 *   GPU nodes use their dotted-decimal IP as node_id.
 *   Switch nodes use "switch_<N>" as node_id.
 *   Example (2 switches, 4 GPUs):
 *     switch_0  10.0.0.1  10.0.0.2  switch_1
 *     switch_1  10.0.0.3  10.0.0.4  switch_0
 *
 * Sanity-test checklist:
 *   [ ] All ranks print "Discovery complete" with consistent IP table
 *   [ ] All ranks print "Peel initialized and ready" (no transport init failure)
 *   [ ] peel_tree logs show expected N hop layers, M subtrees
 *   [ ] Each peel[rank] log shows SYN/ACK completing for all subtree participants
 *   [ ] Rank 0 broadcasts; all others receive; no "FAILED" in output
 *   [ ] Broadcast completes within expected time (no timeout / retransmit storm)
 *   [ ] All ranks print "SUCCESS - Data verified correctly"
 *
 * Examples:
 *   Flat mode (3 ranks, no topology):
 *     ./test_peel_broadcast 0 3 10.161.159.133 6379 vmbr0
 *
 *   Tree mode (3 ranks, with topology):
 *     ./test_peel_broadcast 0 3 10.161.159.133 6379 vmbr0 239.255.0.1 5000 topo.txt
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "gloo/rendezvous/prefix_store.h"
#include "gloo/rendezvous/redis_store.h"
#include "gloo/transport/tcp/device.h"
#include "gloo/transport/tcp/context.h"
#include "gloo/transport/tcp/peel/peel_context.h"
#include "gloo/transport/tcp/peel/peel_discovery.h"

using Clock = std::chrono::steady_clock;

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <rank> <world_size> <redis_host>"
                 " [redis_port] [iface] [mcast_group] [mcast_port] [topology_file]\n";
    std::cerr << "\nDefaults:\n";
    std::cerr << "  redis_port:    6379\n";
    std::cerr << "  iface:         (required for AF_PACKET)\n";
    std::cerr << "  mcast_group:   239.255.0.1\n";
    std::cerr << "  mcast_port:    5000\n";
    std::cerr << "  topology_file: (none — flat single-transport mode)\n";
    std::cerr << "\nExamples:\n";
    std::cerr << "  " << prog << " 0 3 10.161.159.133 6379 vmbr0\n";
    std::cerr << "  " << prog << " 0 3 10.161.159.133 6379 vmbr0 239.255.0.1 5000 topo.txt\n";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    // =========================================================================
    // Step 1: Parse arguments
    // =========================================================================
    int rank = std::atoi(argv[1]);
    int worldSize = std::atoi(argv[2]);
    std::string redisHost    = argv[3];
    int         redisPort    = (argc > 4) ? std::atoi(argv[4]) : 6379;
    std::string iface        = (argc > 5) ? argv[5] : "";
    std::string mcastGroup   = (argc > 6) ? argv[6] : "239.255.0.1";
    uint16_t    mcastPort    = (argc > 7) ? static_cast<uint16_t>(std::atoi(argv[7])) : 5000;
    std::string topologyFile = (argc > 8) ? argv[8] : "";

    std::cout << "\n[PEEL] ====== STEP 1: Config ======\n";
    std::cout << "[PEEL] Rank " << rank << "/" << worldSize
              << " connecting to Redis at " << redisHost << ":" << redisPort << "\n";
    if (!iface.empty())
        std::cout << "[PEEL] Interface:     " << iface << "\n";
    std::cout << "[PEEL] Multicast:     " << mcastGroup << ":" << mcastPort << "\n";
    std::cout << "[PEEL] Topology mode: "
              << (topologyFile.empty() ? "FLAT (no topology file — single transport, no CIDR)"
                                       : "TREE (" + topologyFile + ")")
              << "\n";

    // =========================================================================
    // Step 2: Redis rendezvous store
    // Ranks coordinate through Redis. A fixed prefix ensures all ranks join the
    // same collective even across test runs (change prefix to isolate runs).
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 2: Redis store ======\n";

    auto redisStore = std::make_shared<gloo::rendezvous::RedisStore>(redisHost, redisPort);

    // Fixed prefix so ranks from different test runs don't accidentally merge.
    // Change this prefix between test runs if you see stale data issues.
    std::string prefix = "peel_test_fixed";
    auto store = std::make_shared<gloo::rendezvous::PrefixStore>(prefix, redisStore);

    std::cout << "[PEEL] Rank " << rank << ": Redis prefix = '" << prefix << "'\n";

    // =========================================================================
    // Step 3: TCP device and context
    // Standard Gloo TCP rendezvous — needed to call enablePeel() and
    // peelBroadcast() via the context interface.
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 3: TCP context ======\n";

    if (iface.empty()) {
        std::cerr << "[PEEL] Rank " << rank
                  << ": ERROR — iface argument is required for AF_PACKET raw sockets\n";
        return 1;
    }

    gloo::transport::tcp::attr tcpAttr;
    tcpAttr.iface = iface;
    auto tcpDevice = gloo::transport::tcp::CreateDevice(tcpAttr);
    std::cout << "[PEEL] Rank " << rank << ": TCP device created on " << iface << "\n";

    auto tcpContext = tcpDevice->createContext(rank, worldSize);
    auto* tcpCtx = dynamic_cast<gloo::transport::tcp::Context*>(tcpContext.get());
    if (!tcpCtx) {
        std::cerr << "[PEEL] Rank " << rank << ": ERROR — failed to get TCP context\n";
        return 1;
    }

    // All ranks synchronize here: connect() blocks until every pair has
    // completed the TCP handshake. Once this returns, all ranks are ready.
    tcpCtx->createAndConnectAllPairs(store);
    std::cout << "[PEEL] Rank " << rank << ": TCP all-pairs connected ✓\n";

    // =========================================================================
    // Step 4: PeelDiscovery — build rank→IP map
    // Every rank publishes its interface IP to Redis and reads all others back.
    // This gives us the complete peer_ips map needed by PeelTree and PeelContext.
    // Expected output: each rank prints the full IP table (should be consistent
    // across all ranks — if not, the topology file node IDs may not match).
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 4: PeelDiscovery ======\n";

    gloo::transport::tcp::peel::PeelDiscoveryConfig discoveryConfig;
    discoveryConfig.rank         = rank;
    discoveryConfig.world_size   = worldSize;
    discoveryConfig.iface_name   = iface;
    discoveryConfig.redis_host   = redisHost;
    discoveryConfig.redis_port   = redisPort;
    discoveryConfig.redis_prefix = prefix + "_peel";

    gloo::transport::tcp::peel::PeelDiscovery discovery(discoveryConfig);
    if (!discovery.run()) {
        std::cerr << "[PEEL] Rank " << rank << ": ERROR — discovery failed\n";
        return 1;
    }

    std::cout << "[PEEL] Rank " << rank << ": Discovery complete — rank→IP map:\n";
    for (int r = 0; r < worldSize; ++r)
        std::cout << "[PEEL]   rank " << r << " -> " << discovery.getIp(r) << "\n";

    // =========================================================================
    // Step 5: Enable Peel multicast
    //
    // TREE mode (topology_file set + peer_ips populated):
    //   - PeelTree loads the topology file, BFS from rank 0's IP, builds MST,
    //     partitions into subtrees, assigns a CIDR-encoded dst MAC per subtree.
    //   - Rank 0 creates N transport objects (one per subtree).
    //   - Other ranks create 1 transport (for their own subtree).
    //   - Each transport runs PeelFullMesh: opens AF_PACKET sockets, joins the
    //     CIDR MAC with the NIC, and runs the SYN/SYN-ACK handshake.
    //   - Each transport spawns a worker thread that waits for submitWork().
    //
    // FLAT mode (no topology_file):
    //   - One PeelTransport is created covering all ranks.
    //   - No CIDR rules — standard 01:00:5e: multicast MAC used.
    //   - Same handshake + worker thread as tree mode.
    //
    // Sanity checks:
    //   - peel_tree logs: N hop layers, M subtrees, correct rank assignment
    //   - peel[rank] logs: SYN attempt 1 succeeds; no "timeout" messages
    //   - "peel_context[rank]: initialized (N transport(s) across M subtree(s))"
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 5: Enabling Peel ======\n";

    gloo::transport::tcp::peel::PeelContextConfig peelConfig;
    peelConfig.rank          = rank;
    peelConfig.world_size    = worldSize;
    peelConfig.mcast_group   = mcastGroup;
    peelConfig.base_port     = mcastPort;
    peelConfig.iface_name    = iface;
    peelConfig.topology_file = topologyFile;

    // Pass the discovered rank→IP map so PeelContext/PeelTree can locate nodes
    // in the topology graph and assign ranks to subtrees.
    for (int r = 0; r < worldSize; ++r)
        peelConfig.peer_ips[r] = discovery.getIp(r);

    std::cout << "[PEEL] Rank " << rank << ": PeelContextConfig:\n";
    std::cout << "         mcast_group:   " << peelConfig.mcast_group   << "\n";
    std::cout << "         base_port:     " << peelConfig.base_port
              << "  (subtree k uses ports [base + k*world_size, base + (k+1)*world_size))\n";
    std::cout << "         iface_name:    " << peelConfig.iface_name    << "\n";
    std::cout << "         dscp:          " << (int)peelConfig.dscp
              << "  (written to ip->tos as dscp<<2 = 0x"
              << std::hex << (peelConfig.dscp << 2) << std::dec << ")\n";
    std::cout << "         topology_file: "
              << (peelConfig.topology_file.empty() ? "(none — flat mode)"
                                                   : peelConfig.topology_file)
              << "\n";

    tcpCtx->enablePeel(peelConfig);

    if (!tcpCtx->isPeelReady()) {
        std::cerr << "[PEEL] Rank " << rank
                  << ": ERROR — Peel init failed (check peel_tree / peel_full_mesh logs above)\n";
        return 1;
    }

    std::cout << "[PEEL] Rank " << rank << ": Peel initialized and ready ✓\n";

    // =========================================================================
    // Step 6: Prepare test data
    // Rank 0 fills the buffer with sequential values so any receive corruption
    // can be detected element-by-element.  Others start with zeros.
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 6: Prepare data ======\n";

    const size_t dataCount = 1024 * 1024;          // 1 M uint32 elements = 4 MB
    const size_t dataBytes = dataCount * sizeof(uint32_t);
    std::vector<uint32_t> data(dataCount);
    const int root = 0;

    if (rank == root) {
        std::iota(data.begin(), data.end(), 0);    // 0, 1, 2, ..., N-1
        std::cout << "[PEEL] Rank " << rank << ": SENDER — filled buffer with "
                  << dataCount << " sequential uint32s (" << dataBytes << " bytes)\n";
    } else {
        std::fill(data.begin(), data.end(), 0);
        std::cout << "[PEEL] Rank " << rank << ": RECEIVER — buffer zeroed, waiting\n";
    }

    // =========================================================================
    // Step 7: Peel broadcast
    //
    // Internally (PeelBroadcast::run):
    //   Phase 1 — submitWork() called on every transport in sequence (each wakes
    //             its worker thread; workers start executing concurrently).
    //   Phase 2 — waitResult() called on each transport in sequence (blocks until
    //             that worker finishes; total wall time = max(subtree times)).
    //
    // For rank 0: each worker calls send(), which chunks the buffer into
    //   PEEL_MAX_PAYLOAD packets, fires them as raw AF_PACKET UDP frames with
    //   CIDR dst MAC, and waits for stop-and-wait ACKs from each receiver.
    //
    // For non-zero ranks: the single worker calls recv(from_rank=0), reads all
    //   chunks, and sends unicast ACKs back.
    //
    // Expected log pattern (rank 0):
    //   peel_transport[0]: ready  (repeated for each subtree transport)
    //   (no timeout / retransmit messages for a healthy network)
    //
    // Expected log pattern (non-zero rank):
    //   peel_transport[R]: ready
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 7: Broadcast ======\n";

    auto startTime = Clock::now();
    bool ok = tcpCtx->peelBroadcast(root, data.data(), dataBytes);
    auto endTime   = Clock::now();

    if (!ok) {
        std::cerr << "[PEEL] Rank " << rank
                  << ": ERROR — broadcast returned false "
                     "(check for timeout / socket errors above)\n";
        return 1;
    }

    // =========================================================================
    // Step 8: Report timing
    // =========================================================================
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime).count();
    double durationMs       = durationUs / 1000.0;
    double throughputMBps   = static_cast<double>(dataBytes) /
                              (durationUs / 1e6) / (1024.0 * 1024.0);

    std::cout << "\n[PEEL] ====== STEP 8: Timing ======\n";
    std::cout << "[PEEL] Rank " << rank << ": broadcast completed in "
              << durationMs << " ms  (" << throughputMBps << " MB/s)\n";

    // =========================================================================
    // Step 9: Verify data
    // All ranks (including sender) verify their buffer matches the expected
    // sequence.  Receiver corruption typically shows up as a mismatch at the
    // exact byte boundary of a dropped/reordered packet.
    // =========================================================================
    std::cout << "\n[PEEL] ====== STEP 9: Verify data ======\n";

    bool success = true;
    for (size_t i = 0; i < dataCount; ++i) {
        if (data[i] != static_cast<uint32_t>(i)) {
            std::cerr << "[PEEL] Rank " << rank
                      << ": VERIFICATION FAILED at index " << i
                      << " — expected " << i << ", got " << data[i]
                      << "  (possible partial receive or packet reorder)\n";
            success = false;
            break;
        }
    }

    if (success)
        std::cout << "[PEEL] Rank " << rank << ": SUCCESS — all " << dataCount
                  << " values verified correctly ✓\n";

    std::cout << "[PEEL] Rank " << rank << ": Exiting\n\n";
    return success ? 0 : 1;
}
