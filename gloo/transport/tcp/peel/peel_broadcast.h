// gloo/transport/tcp/peel/peel_broadcast.h

#pragma once

#include "peel_transport.h"

#include <cstddef>
#include <memory>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// Simple broadcast using PeelTransport
class PeelBroadcast {
public:
    explicit PeelBroadcast(PeelTransport* transport);

    // Broadcast buffer from root to all ranks
    bool run(int root, void* data, size_t size);

private:
    PeelTransport* transport_;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo