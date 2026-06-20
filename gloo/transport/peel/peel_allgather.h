// gloo/transport/peel/peel_allgather.h

#pragma once

#include "peel_context.h"

#include <cstddef>
#include <vector>

namespace gloo {
namespace transport {
namespace peel {

// Controls whether the N broadcasts that make up the allgather run one after
// another or all at once.
enum class PeelAllgatherMode {
    // Each broadcast completes before the next starts.
    // Easier to debug; total time = sum(broadcast_times).
    Sequential,

    // All N broadcasts are submitted simultaneously in separate threads.
    // Total time = max(broadcast_times) instead of sum.
    // Safe because each context uses non-overlapping port ranges.
    Parallel,
};

// Allgather built on top of PeelBroadcast.
//
// Owns N PeelContext pointers — one per sender rank — and runs one broadcast
// per context so that after run() every rank holds every other rank's data.
//
// Usage:
//   // contexts[r] must already be init()'d with sender_rank == r.
//   PeelAllgather ag(contexts, PeelAllgatherMode::Parallel);
//   ag.run(bufs, dataBytes);   // bufs[r] = pointer to rank r's buffer
//
class PeelAllgather {
public:
    // contexts[r] must be a PeelContext whose sender_rank == r.
    // All contexts must be init()'d and isReady() before calling run().
    explicit PeelAllgather(std::vector<PeelContext*> contexts,
                           PeelAllgatherMode mode = PeelAllgatherMode::Sequential);

    // Run the allgather.
    //   bufs[r]  — pointer to the buffer for rank r
    //   size     — bytes per buffer (same for every rank)
    //
    // After run() returns true, every bufs[r] on every node holds rank r's
    // data.  Returns false if any broadcast fails.
    bool run(const std::vector<void*>& bufs, size_t size);

    PeelAllgatherMode mode() const { return mode_; }
    int               numRanks() const { return static_cast<int>(contexts_.size()); }

private:
    bool runSequential(const std::vector<void*>& bufs, size_t size);
    bool runParallel  (const std::vector<void*>& bufs, size_t size);

    std::vector<PeelContext*> contexts_;
    PeelAllgatherMode         mode_;
};

} // namespace peel
} // namespace transport
} // namespace gloo
