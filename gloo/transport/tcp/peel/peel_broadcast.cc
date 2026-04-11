// gloo/transport/tcp/peel/peel_broadcast.cc

#include "peel_broadcast.h"

#include <iostream>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

PeelBroadcast::PeelBroadcast(std::vector<PeelTransport*> transports)
    : transports_(std::move(transports)) {}

PeelBroadcast::PeelBroadcast(PeelTransport* transport)
    : transports_{transport} {}

bool PeelBroadcast::run(int root, void* data, size_t size) {
    if (transports_.empty()) {
        std::cerr << "peel_broadcast: no transports\n";
        return false;
    }

    // Validate every transport before touching any of them.
    for (auto* t : transports_) {
        if (!t || !t->isReady()) {
            std::cerr << "peel_broadcast: a transport is not ready\n";
            return false;
        }
    }

    // Phase 1: fire all worker threads simultaneously.
    // submitWork() returns immediately after waking the worker, so by the time
    // the last submitWork() returns all workers are already executing in parallel.
    for (auto* t : transports_)
        t->submitWork(root, data, size);

    // Phase 2: collect results.
    // Each waitResult() blocks until that subtree's worker finishes.
    // Because the workers are already running in parallel, the total wall-clock
    // time here is max(subtree_times), not sum(subtree_times).
    bool ok = true;
    for (auto* t : transports_)
        ok &= t->waitResult();

    return ok;
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo