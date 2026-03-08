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

constexpr uint16_t PEEL_BASE_PORT = 50000;

// Configuration for full-mesh multicast setup
struct PeelFullMeshConfig {
    // Multicast settings
    std::string mcast_group = "239.255.0.1";
    uint16_t base_port = PEEL_BASE_PORT;
    
    // Process identity
    int rank = 0;
    int world_size = 1;
    
    // Network settings
    std::string iface_ip;                 // Interface IP (empty = default)
    int ttl = 1;                          // Multicast TTL
    int rcvbuf = 4 * 1024 * 1024;         // Receive buffer size
    
    // Timing settings
    int rto_ms = 250;                     // Retransmit timeout
    int handshake_timeout_ms = 30000;     // Total handshake timeout
    int poll_interval_ms = 50;            // Redis poll interval
    
    // Redis settings
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_prefix = "peel";    // Key prefix in Redis
    
    // Computed helpers
    uint16_t sendPort() const { return base_port + rank; }
    uint16_t recvPort(int sender_rank) const { return base_port + sender_rank; }
};

// Represents one multicast channel (one sender, multiple receivers)
struct PeelChannel {
    int fd = -1;                          // Socket file descriptor
    int sender_rank = -1;                 // Which rank owns this channel (sends on it)
    uint16_t port = 0;                    // Port number
    sockaddr_in mcast{};                  // Multicast destination address
    bool is_send_channel = false;         // True if this rank sends on this channel

    ~PeelChannel();
    PeelChannel() = default;
    PeelChannel(PeelChannel&& other) noexcept;
    PeelChannel& operator=(PeelChannel&& other) noexcept;
    PeelChannel(const PeelChannel&) = delete;
    PeelChannel& operator=(const PeelChannel&) = delete;
    
    // Release ownership of fd
    int releaseFd();
};

// Result of successful full-mesh handshake
struct PeelFullMeshResult {
    int rank = -1;
    int world_size = 0;
    
    // Channel where THIS rank sends (one)
    std::unique_ptr<PeelChannel> send_channel;
    
    // Channels where THIS rank receives (world_size - 1)
    std::vector<std::unique_ptr<PeelChannel>> recv_channels;
    
    // Peer information discovered during handshake
    // Index = rank, value = peer's address
    std::vector<sockaddr_in> peers;
    
    // Get recv channel by sender rank (returns nullptr if not found)
    PeelChannel* getRecvChannel(int sender_rank);
};

// Main class for setting up full-mesh multicast
class PeelFullMesh {
public:
    explicit PeelFullMesh(const PeelFullMeshConfig& config);
    ~PeelFullMesh();

    PeelFullMesh(const PeelFullMesh&) = delete;
    PeelFullMesh& operator=(const PeelFullMesh&) = delete;

    // Step 1: Initialize - connect to Redis, create sockets
    bool init();
    
    // Step 2: Run full handshake (barrier + SYN/ACK exchange)
    // Returns result on success, nullptr on failure
    std::unique_ptr<PeelFullMeshResult> run();
    
    // Cleanup Redis keys (call after all ranks done, optional)
    void cleanup();

private:
    // Internal steps
    bool connectRedis();
    bool createSockets();
    bool signalReady();
    bool waitForAllReady();
    bool performHandshake(PeelFullMeshResult& result);
    
    // Socket helpers
    int createSocket(uint16_t port, bool is_sender);
    void setNonBlocking(int fd);
    void setRecvTimeout(int fd, int timeout_ms);
    
    // Packet helpers
    void fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags, uint8_t retrans_id);
    bool sendPacket(int fd, const sockaddr_in& dest, const PeelHeader& hdr);
    
    // Receive with timeout (non-blocking)
    // Returns number of bytes received, 0 on timeout, -1 on error
    ssize_t recvPacketNonBlocking(int fd, sockaddr_in& from, PeelHeader& hdr, int timeout_ms);

    PeelFullMeshConfig config_;
    std::unique_ptr<PeelRedis> redis_;
    
    // Sockets
    int send_fd_ = -1;                    // Send socket for this rank's channel
    std::vector<int> recv_fds_;           // Receive sockets, indexed by sender rank
    
    // Multicast address (base, port filled per channel)
    sockaddr_in mcast_base_{};
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo