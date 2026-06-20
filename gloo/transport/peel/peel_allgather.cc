// gloo/transport/peel/peel_allgather.cc

#include "peel_allgather.h"

#include <atomic>
#include <iostream>
#include <thread>

namespace gloo {
namespace transport {
namespace peel {

PeelAllgather::PeelAllgather(std::vector<PeelContext*> contexts,
                             PeelAllgatherMode mode)
    : contexts_(std::move(contexts)), mode_(mode) {}

bool PeelAllgather::run(const std::vector<void*>& bufs, size_t size) {
    if (bufs.size() != contexts_.size()) {
        std::cerr << "[PeelAllgather] bufs.size() " << bufs.size()
                  << " != contexts_.size() " << contexts_.size() << "\n";
        return false;
    }
    for (size_t r = 0; r < contexts_.size(); ++r) {
        if (!contexts_[r] || !contexts_[r]->isReady()) {
            std::cerr << "[PeelAllgather] context[" << r << "] is null or not ready\n";
            return false;
        }
    }

    return (mode_ == PeelAllgatherMode::Parallel)
               ? runParallel(bufs, size)
               : runSequential(bufs, size);
}

// -----------------------------------------------------------------------------
// Sequential: broadcast r finishes before broadcast r+1 starts.
// Total time = sum(broadcast_times).
// -----------------------------------------------------------------------------
bool PeelAllgather::runSequential(const std::vector<void*>& bufs, size_t size) {
    for (int r = 0; r < static_cast<int>(contexts_.size()); ++r) {
        if (!contexts_[r]->broadcast(r, bufs[r], size)) {
            std::cerr << "[PeelAllgather] sequential broadcast failed for rank " << r << "\n";
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Parallel: all N broadcasts run concurrently in separate threads.
// Total time = max(broadcast_times).
//
// Each context uses a non-overlapping port range (base + r*W*W + ...) so
// there are no port conflicts between the concurrent broadcasts.
// -----------------------------------------------------------------------------
bool PeelAllgather::runParallel(const std::vector<void*>& bufs, size_t size) {
    const int n = static_cast<int>(contexts_.size());
    std::vector<std::thread> threads;
    threads.reserve(n);
    std::atomic<bool> ok{true};

    for (int r = 0; r < n; ++r) {
        threads.emplace_back([this, r, &bufs, size, &ok]() {
            if (!contexts_[r]->broadcast(r, bufs[r], size)) {
                std::cerr << "[PeelAllgather] parallel broadcast failed for rank " << r << "\n";
                ok.store(false, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return ok.load();
}

} // namespace peel
} // namespace transport
} // namespace gloo
