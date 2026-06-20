#pragma once

#include "peel_transport.h"

#include <cstddef>
#include <vector>

namespace gloo {
namespace transport {
namespace peel {

struct PeelTreeHop {
    int parent = -1;
    int child = -1;
    size_t level = 0;
    PeelTransport* transport = nullptr;
};

class PeelBroadcastStopAndWait {
public:
    PeelBroadcastStopAndWait(
        int rank,
        int worldSize,
        int root,
        std::vector<PeelTreeHop> hops);

    // Tree broadcast that mirrors gloo/broadcast.cc, but each logical
    // parent -> child edge uses a dedicated 2-rank PeelTransport.
    bool run(int root, void* data, size_t size);

private:
    const int rank_;
    const int worldSize_;
    const int root_;
    std::vector<PeelTreeHop> hops_;
};

} // namespace peel
} // namespace transport
} // namespace gloo
