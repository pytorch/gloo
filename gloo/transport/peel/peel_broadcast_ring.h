#pragma once

#include "peel_transport.h"

#include <cstddef>
#include <vector>

namespace gloo {
namespace transport {
namespace peel {

struct PeelRingHop {
    int sender = -1;
    int receiver = -1;
    PeelTransport* transport = nullptr;
};

class PeelBroadcastRing {
public:
    explicit PeelBroadcastRing(std::vector<PeelRingHop> hops);

    // Ring broadcast from root across sequential 2-rank Peel transports.
    bool run(int root, void* data, size_t size);

private:
    std::vector<PeelRingHop> hops_;
};

} // namespace peel
} // namespace transport
} // namespace gloo