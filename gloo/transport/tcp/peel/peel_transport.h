// gloo/transport/tcp/peel/peel_transport.h

#pragma once

#include "peel_full_mesh.h"
#include "peel_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// =============================================================================
// Transport Configuration
// =============================================================================

struct PeelTransportConfig {
    // From PeelFullMeshConfig
    std::string mcast_group = "239.255.0.1";
    uint16_t base_port = PEEL_DEFAULT_BASE_PORT;
    int rank = 0;
    int world_size = 1;
    std::string iface_name;
    int ttl = PEEL_DEFAULT_TTL;
    int rcvbuf = 4 * 1024 * 1024;
    int rto_ms = PEEL_DEFAULT_RTO_MS;
    int timeout_ms = PEEL_DEFAULT_TIMEOUT_MS;

    // Redis
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_prefix = "peel";

    // Data transfer
    size_t max_chunk_size = PEEL_MAX_PAYLOAD;
};

// =============================================================================
// Peel Transport
// =============================================================================

class PeelTransport {
public:
    explicit PeelTransport(const PeelTransportConfig& config);
    ~PeelTransport();

    PeelTransport(const PeelTransport&) = delete;
    PeelTransport& operator=(const PeelTransport&) = delete;

    // Initialize and perform handshake
    bool init();

    // Check if ready
    bool isReady() const { return mesh_result_ != nullptr; }

    // Get identity
    int rank() const { return config_.rank; }
    int worldSize() const { return config_.world_size; }

    // =========================================================================
    // Broadcast API
    // =========================================================================

    // Broadcast data from root to all other ranks
    // - root: rank that sends
    // - data: buffer (root sends, others receive)
    // - size: number of bytes
    // Returns true on success
    bool broadcast(int root, void* data, size_t size);

    // =========================================================================
    // Low-level Send/Recv (for future extensions)
    // =========================================================================

    // Send data on this rank's multicast channel (to all)
    bool send(const void* data, size_t size);

    // Receive data from a specific rank
    // timeout_ms: -1 = blocking, 0 = non-blocking, >0 = timeout
    // Returns bytes received, 0 on timeout, -1 on error
    ssize_t recv(int from_rank, void* data, size_t max_size, int timeout_ms = -1);

    // =========================================================================
    // Cleanup
    // =========================================================================

    void cleanup();

private:
    // Send a single packet
    bool sendPacket(uint32_t seq, uint16_t flags, const void* payload, size_t len);

    // Receive a packet from specific rank
    bool recvPacket(int from_rank, PeelHeader& hdr,
                    std::vector<uint8_t>& payload, int timeout_ms);

    // Wait for ACKs from all receivers (stop-and-wait)
    bool waitForAcks(uint32_t seq, int timeout_ms);

    // Send a unicast ACK frame back to the sender
    void sendAck(uint32_t dst_ip_n, uint16_t dst_port_h, const uint8_t dst_mac[6],
                 uint32_t seq, uint32_t tsecr, uint8_t retrans_id);

    PeelTransportConfig config_;
    std::unique_ptr<PeelFullMeshResult> mesh_result_;
    uint32_t next_seq_ = 1;
    uint16_t ip_id_    = 0;  // Rolling IP identification counter
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo