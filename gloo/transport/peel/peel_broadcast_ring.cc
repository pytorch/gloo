#include "peel_broadcast_ring.h"

#include <iostream>

namespace gloo {
namespace transport {
namespace peel {

PeelBroadcastRing::PeelBroadcastRing(std::vector<PeelRingHop> hops)
    : hops_(std::move(hops)) {}

bool PeelBroadcastRing::run(int root, void* data, size_t size) {
    const int worldSize = static_cast<int>(hops_.size());

    if (worldSize <= 1) {
        return true;
    }
    if (root < 0 || root >= worldSize) {
        std::cerr << "peel_broadcast_ring: invalid root " << root
                  << " for world size " << worldSize << "\n";
        return false;
    }

    // hops_[i] represents the logical edge i -> (i + 1) % worldSize.
    // On each process, only the transports for hops that involve this rank are
    // created. Other hop entries have transport == nullptr and are skipped.
    for (int step = 0; step < worldSize - 1; ++step) {
        const int idx = (root + step) % worldSize;
        const auto& hop = hops_[idx];

        if (hop.transport == nullptr) {
            continue;
        }
        if (!hop.transport->isReady()) {
            std::cerr << "peel_broadcast_ring: hop transport not ready: "
                      << hop.sender << " -> " << hop.receiver << "\n";
            return false;
        }
        if (!hop.transport->broadcast(hop.sender, data, size)) {
            std::cerr << "peel_broadcast_ring: hop failed: "
                      << hop.sender << " -> " << hop.receiver << "\n";
            return false;
        }
    }

    return true;
}

} // namespace peel
} // namespace transport
} // namespace gloo