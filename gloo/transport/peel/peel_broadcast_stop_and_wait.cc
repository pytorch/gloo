#include "peel_broadcast_stop_and_wait.h"

#include <iostream>
#include <utility>

#include "gloo/math.h"

namespace gloo {
namespace transport {
namespace peel {

PeelBroadcastStopAndWait::PeelBroadcastStopAndWait(
    int rank,
    int worldSize,
    int root,
    std::vector<PeelTreeHop> hops)
    : rank_(rank),
      worldSize_(worldSize),
      root_(root),
      hops_(std::move(hops)) {}

bool PeelBroadcastStopAndWait::run(int root, void* data, size_t size) {
    if (worldSize_ <= 1 || size == 0) {
        return true;
    }

    if (root != root_) {
        std::cerr << "peel_broadcast_stop_and_wait: runtime root " << root
                  << " does not match initialized root " << root_ << "\n";
        return false;
    }

    const size_t dim = gloo::log2ceil(static_cast<uint32_t>(worldSize_));

    for (size_t level = 0; level < dim; ++level) {
        for (const auto& hop : hops_) {
            if (hop.level != level) {
                continue;
            }

            if (hop.transport == nullptr) {
                continue;
            }

            if (!hop.transport->isReady()) {
                std::cerr << "peel_broadcast_stop_and_wait: transport not ready for "
                          << hop.parent << " -> " << hop.child << "\n";
                return false;
            }

            if (!hop.transport->broadcast(hop.parent, data, size)) {
                std::cerr << "peel_broadcast_stop_and_wait: hop failed for "
                          << hop.parent << " -> " << hop.child
                          << " at level " << hop.level << "\n";
                return false;
            }

            // A rank participates in at most one edge per tree level.
            break;
        }
    }

    return true;
}

} // namespace peel
} // namespace transport
} // namespace gloo
