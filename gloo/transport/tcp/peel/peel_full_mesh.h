// gloo/transport/tcp/peel/peel_full_mesh.h

#pragma once

#include "peel_protocol.h"

#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <linux/if_packet.h>
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

    // Subset of ranks that participate in this mesh.
    // Empty means all ranks 0..world_size-1 (default / single-transport case).
    // When PeelTree is used, each subtree sets this to its own receiver_ranks.
    std::vector<int> participant_ranks;

    // Network — interface name only; MAC and IP are derived automatically
    std::string iface_name;   // e.g. "eth0"
    int ttl   = PEEL_DEFAULT_TTL;
    int rcvbuf = 4 * 1024 * 1024;

    // CIDR rules: if use_cidr_rules_mac is true, every outgoing frame
    // (handshake and data) uses cidr_rules_mac as the Ethernet destination
    // instead of the standard derived multicast MAC.
    uint8_t cidr_rules_mac[6]  = {};
    bool    use_cidr_rules_mac = false;

    // DSCP (Differentiated Services Code Point) for outgoing multicast packets.
    // Written directly to ip->tos as (dscp << 2) in the raw IP header.
    // Default 7 = CS1 (low-drop precedence, bulk traffic).
    uint8_t dscp = 7;

    // Rank that acts as the data sender for this mesh.
    // During handshake: sender multicasts SYN and waits for unicast ACKs from
    // all receivers, then broadcasts START.  Receivers only listen for SYN from
    // this rank, unicast-ACK back, and wait for START — they never multicast.
    // For broadcast this is the broadcast root (default 0).
    int sender_rank = 0;

    // Timing
    int rto_ms = PEEL_DEFAULT_RTO_MS;
    int handshake_timeout_ms = PEEL_DEFAULT_TIMEOUT_MS;

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
    sockaddr_in mcast{};      // Multicast IP destination (for building IP header)
    sockaddr_ll ll_dest{};    // L2 destination (for sendto on AF_PACKET)
    bool is_sender = false;

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

    // Interface info — derived at init, needed by PeelTransport for frame building
    uint32_t src_ip_n = 0;      // This rank's unicast IP (network order)
    uint8_t  src_mac[6]{};      // This rank's interface MAC
    int      if_idx = 0;        // Interface index

    // This rank's send channel
    std::unique_ptr<PeelChannel> send_channel;

    // Receive channels (from other ranks)
    std::vector<std::unique_ptr<PeelChannel>> recv_channels;

    // Peer addresses discovered during handshake
    std::vector<sockaddr_in> peers;  // Indexed by rank

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

    // Initialize (resolve interface, create sockets)
    bool init();

    // Run handshake (SYN/ACK across participant_ranks)
    std::unique_ptr<PeelFullMeshResult> run();

private:
    bool createSockets();
    bool performHandshake(PeelFullMeshResult& result);

    int  createSocket(uint16_t port, bool is_sender);
    void fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags, uint8_t retrans_id);
    // dst_mac_override: when non-null, used as Ethernet destination instead of
    // the CIDR/multicast MAC.  Pass sender's learned MAC for unicast ACKs.
    bool sendPacket(int fd, const sockaddr_in& dest, const PeelHeader& hdr,
                    const uint8_t* dst_mac_override = nullptr);

    PeelFullMeshConfig config_;

    // Derived from iface_name at init()
    int      if_idx_   = 0;
    uint8_t  src_mac_[6]{};
    uint32_t src_ip_n_ = 0;
    uint16_t ip_id_    = 0;   // Rolling IP identification counter

    int send_fd_ = -1;
    std::vector<int> recv_fds_;
    sockaddr_in mcast_base_{};
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
