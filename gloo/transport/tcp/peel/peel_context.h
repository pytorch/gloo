// gloo/transport/tcp/peel/peel_context.h

#pragma once

#include "peel_transport.h"

#include <memory>
#include <string>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// Configuration for Peel-enabled context
struct PeelContextConfig {
    // Identity
    int rank = 0;
    int world_size = 1;

    // Multicast
    std::string mcast_group = "239.255.0.1";
    uint16_t base_port = PEEL_DEFAULT_BASE_PORT;
    std::string iface_name;
    int ttl = PEEL_DEFAULT_TTL;

    // Redis
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_prefix = "peel";

    // Timing
    int timeout_ms = PEEL_DEFAULT_TIMEOUT_MS;
    int rto_ms = PEEL_DEFAULT_RTO_MS;

    // Buffer
    int rcvbuf = 4 * 1024 * 1024;
    size_t max_chunk_size = PEEL_MAX_PAYLOAD;
};

// Wrapper to create PeelTransport for use with Gloo
class PeelContext {
public:
    explicit PeelContext(const PeelContextConfig& config);
    ~PeelContext();

    PeelContext(const PeelContext&) = delete;
    PeelContext& operator=(const PeelContext&) = delete;

    // Initialize transport (handshake)
    bool init();

    // Check if ready
    bool isReady() const;

    // Get transport for collective operations
    PeelTransport* transport() { return transport_.get(); }
    const PeelTransport* transport() const { return transport_.get(); }

    // Convenience: broadcast
    bool broadcast(int root, void* data, size_t size);

    // Cleanup
    void cleanup();

    // Accessors
    int rank() const { return config_.rank; }
    int worldSize() const { return config_.world_size; }

private:
    PeelContextConfig config_;
    std::unique_ptr<PeelTransport> transport_;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo