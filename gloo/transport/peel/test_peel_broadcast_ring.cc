/**
 * End-to-end sanity test for standalone Peel ring broadcast.
 *
 * Usage:
 *   ./test_peel_broadcast_ring <rank> <world_size> <redis_host>
 *                              [redis_port] [iface] [mcast_group] [base_port]
 *                              [topology_file]
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "gloo/transport/peel/peel_context.h"
#include "gloo/transport/peel/peel_discovery.h"

using Clock = std::chrono::steady_clock;
namespace peel = gloo::transport::peel;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "Usage: " << argv[0]
              << " <rank> <world_size> <redis_host>"
                 " [redis_port] [iface] [mcast_group] [base_port] [topology_file]\n";
    return 1;
  }

  const int rank = std::atoi(argv[1]);
  const int worldSize = std::atoi(argv[2]);
  const std::string redisHost = argv[3];
  const int redisPort = (argc > 4) ? std::atoi(argv[4]) : 6379;
  const std::string iface = (argc > 5) ? argv[5] : "";
  const std::string mcastGroup = (argc > 6) ? argv[6] : "239.255.0.1";
  const uint16_t basePort =
      (argc > 7) ? static_cast<uint16_t>(std::atoi(argv[7])) : 5000;
  const std::string topologyFile = (argc > 8) ? argv[8] : "";

  if (iface.empty()) {
    std::cerr << "[PEEL] ERROR: iface is required for AF_PACKET raw sockets\n";
    return 1;
  }

  peel::PeelDiscoveryConfig discoveryConfig;
  discoveryConfig.rank = rank;
  discoveryConfig.world_size = worldSize;
  discoveryConfig.redis_host = redisHost;
  discoveryConfig.redis_port = redisPort;
  discoveryConfig.redis_prefix = "peel_ring_test_fixed/peel_ip";
  discoveryConfig.iface_name = iface;

  peel::PeelDiscovery discovery(discoveryConfig);
  if (!discovery.run()) {
    std::cerr << "[PEEL] ERROR: discovery failed\n";
    return 2;
  }

  peel::PeelContextConfig peelConfig;
  peelConfig.rank = rank;
  peelConfig.world_size = worldSize;
  peelConfig.peer_ips = discovery.peerIps();
  peelConfig.mcast_group = mcastGroup;
  peelConfig.base_port = basePort;
  peelConfig.iface_name = iface;
  peelConfig.topology_file = topologyFile;

  peel::PeelContext peelContext(peelConfig);
  if (!peelContext.initRing()) {
    std::cerr << "[PEEL] ERROR: PeelContext ring init failed\n";
    return 3;
  }

  constexpr size_t kCount = 1024 * 1024;
  const size_t bytes = kCount * sizeof(uint32_t);
  constexpr int kRoot = 0;
  std::vector<uint32_t> data(kCount);

  if (rank == kRoot) {
    std::iota(data.begin(), data.end(), 0);
  } else {
    std::fill(data.begin(), data.end(), 0);
  }

  const auto start = Clock::now();
  const bool ok = peelContext.broadcastRing(kRoot, data.data(), bytes);
  const auto end = Clock::now();
  if (!ok) {
    std::cerr << "[PEEL] ERROR: ring broadcast failed\n";
    return 4;
  }

  const auto usec =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count();
  std::cout << "[PEEL] ring broadcast completed in " << usec / 1000.0
            << " ms\n";

  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] != static_cast<uint32_t>(i)) {
      std::cerr << "[PEEL] verification failed at " << i << ": got "
                << data[i] << " expected " << i << "\n";
      return 5;
    }
  }

  peelContext.cleanup();
  std::cout << "[PEEL] SUCCESS\n";
  return 0;
}
