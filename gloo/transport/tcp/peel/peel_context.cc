// gloo/transport/tcp/peel/peel_context.cc

#include "peel_context.h"

#include <iostream>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

PeelContext::PeelContext(const PeelContextConfig& config)
    : config_(config) {}

PeelContext::~PeelContext() {
    cleanup();
}

bool PeelContext::init() {
    PeelTransportConfig tc;
    tc.mcast_group = config_.mcast_group;
    tc.base_port = config_.base_port;
    tc.rank = config_.rank;
    tc.world_size = config_.world_size;
    tc.iface_ip = config_.iface_ip;
    tc.ttl = config_.ttl;
    tc.rcvbuf = config_.rcvbuf;
    tc.rto_ms = config_.rto_ms;
    tc.timeout_ms = config_.timeout_ms;
    tc.redis_host = config_.redis_host;
    tc.redis_port = config_.redis_port;
    tc.redis_prefix = config_.redis_prefix;
    tc.max_chunk_size = config_.max_chunk_size;

    transport_ = std::make_unique<PeelTransport>(tc);

    if (!transport_->init()) {
        std::cerr << "peel_context: transport init failed\n";
        transport_.reset();
        return false;
    }

    std::cerr << "peel_context[" << config_.rank << "]: initialized\n";
    return true;
}

bool PeelContext::isReady() const {
    return transport_ && transport_->isReady();
}

bool PeelContext::broadcast(int root, void* data, size_t size) {
    if (!isReady()) return false;
    return transport_->broadcast(root, data, size);
}

void PeelContext::cleanup() {
    if (transport_) {
        transport_->cleanup();
        transport_.reset();
    }
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo