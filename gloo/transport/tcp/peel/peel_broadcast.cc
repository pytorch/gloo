// gloo/transport/tcp/peel/peel_broadcast.cc

#include "peel_broadcast.h"

#include <iostream>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

PeelBroadcast::PeelBroadcast(PeelTransport* transport)
    : transport_(transport) {}

bool PeelBroadcast::run(int root, void* data, size_t size) {
    if (!transport_ || !transport_->isReady()) {
        std::cerr << "peel_broadcast: transport not ready\n";
        return false;
    }

    return transport_->broadcast(root, data, size);
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo