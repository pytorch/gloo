// gloo/transport/tcp/peel/peel_broadcast.h

#pragma once

#include "peel_transport.h"

#include <cstddef>
#include <vector>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// Broadcast across one or more subtree transports.
//
// When multiple transports are provided (one per subtree), run() uses a
// two-phase approach so all worker threads execute concurrently:
//
//   Phase 1 — submitWork() on every transport  (non-blocking, fires all workers)
//   Phase 2 — waitResult() on every transport  (collects when each finishes)
//
// Total wall-clock time is max(subtree_times) instead of sum(subtree_times).
// With a single transport the behaviour is identical to the old implementation.
class PeelBroadcast {
public:
    // Primary constructor: one entry per subtree transport.
    explicit PeelBroadcast(std::vector<PeelTransport*> transports);

    // Convenience constructor for the single-transport case.
    explicit PeelBroadcast(PeelTransport* transport);

    // Broadcast buffer from root to all ranks across all subtrees.
    // Returns true only if every subtree transport succeeds.
    bool run(int root, void* data, size_t size);

private:
    std::vector<PeelTransport*> transports_;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo