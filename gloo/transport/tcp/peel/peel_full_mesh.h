#pragma once

#include "peel_protocol.h"
#include "peel_redis.h"

#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// =============================================================================
// Configuration
// =============================================================================

struct PeelFullMeshConfig {
    // Multicast
    std::string mcast_group = "239.255.0.1";
    uint16_t base_port = PEEL_DEFAULT_BASE_PORT;

    // Identity
    int rank = 0;
    int world_size = 1;

    // Network
    std::string iface_ip;
    int ttl = PEEL_DEFAULT_TTL;
    int rcvbuf = 4 * 1024 * 1024;

    // Timing
    int rto_ms = PEEL_DEFAULT_RTO_MS;
    int handshake_timeout_ms = PEEL_DEFAULT_TIMEOUT_MS;
    int poll_interval_ms = 50;

    // Redis
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_prefix = "peel";

    // Helpers
    uint16_t sendPort() const { return base_port + static_cast<uint16_t>(rank); }
    uint16_t recvPort(int r) const { return base_port + static_cast<uint16_t>(r); }
};

// =============================================================================
// Channel (one per rank)
// =============================================================================

struct PeelChannel {
    int fd = -1;
    int owner_rank = -1;      // Which rank sends on this channel
    uint16_t port = 0;
    sockaddr_in mcast{};
    bool is_sender = false;   // True if this rank owns (sends on) this channel

    ~PeelChannel();
    PeelChannel() = default;
    PeelChannel(PeelChannel&& o) noexcept;
    PeelChannel& operator=(PeelChannel&& o) noexcept;
    PeelChannel(const PeelChannel&) = delete;
    PeelChannel& operator=(const PeelChannel&) = delete;

    int releaseFd();
    bool isValid() const { return fd >= 0; }
};

// =============================================================================
// Result
// =============================================================================

struct PeelFullMeshResult {
    int rank = -1;
    int world_size = 0;

    // This rank's send channel
    std::unique_ptr<PeelChannel> send_channel;

    // Receive channels (from other ranks)
    std::vector<std::unique_ptr<PeelChannel>> recv_channels;

    // Peer addresses discovered during handshake
    std::vector<sockaddr_in> peers;  // Indexed by rank

    // Get receive channel for a specific sender
    PeelChannel* getRecvChannel(int sender_rank);
    const PeelChannel* getRecvChannel(int sender_rank) const;
};

// =============================================================================
// Full Mesh Setup
// =============================================================================

class PeelFullMesh {
public:
    explicit PeelFullMesh(const PeelFullMeshConfig& config);
    ~PeelFullMesh();

    PeelFullMesh(const PeelFullMesh&) = delete;
    PeelFullMesh& operator=(const PeelFullMesh&) = delete;

    // Initialize (connect Redis, create sockets)
    bool init();

    // Run handshake (barrier + SYN/ACK)
    std::unique_ptr<PeelFullMeshResult> run();

    // Cleanup Redis keys
    void cleanup();

private:
    bool connectRedis();
    bool createSockets();
    bool signalReady();
    bool waitForAllReady();
    bool performHandshake(PeelFullMeshResult& result);

    int createSocket(uint16_t port, bool is_sender);
    void fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags, uint8_t retrans_id);
    bool sendPacket(int fd, const sockaddr_in& dest, const PeelHeader& hdr);

    PeelFullMeshConfig config_;
    std::unique_ptr<PeelRedis> redis_;

    int send_fd_ = -1;
    std::vector<int> recv_fds_;
    sockaddr_in mcast_base_{};
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo