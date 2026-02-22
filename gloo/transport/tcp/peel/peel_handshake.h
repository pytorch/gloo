#pragma once

#include "gloo/transport/tcp/peel/peel_protocol.h"

#include <cstdint>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// Configuration for Peel handshake
struct PeelConfig {
    std::string group = "239.255.0.1";   // multicast group address
    uint16_t mcast_port = 5000;          // multicast destination port
    uint16_t local_port = 0;             // local bind port (0 = ephemeral)
    std::string iface_ip;                // egress interface IP (empty = default)
    int ttl = 1;                         // multicast TTL
    int rto_ms = 250;                    // retransmission timeout in ms
    int retries = 20;                    // max retries per step
    int handshake_timeout_ms = 30000;    // total handshake timeout for receiver
};

// Result of successful handshake
struct PeelCohort {
    std::vector<sockaddr_in> peers;      // All peers discovered
    int fd = -1;                         // UDP socket (caller takes ownership)
    sockaddr_in mcast{};                 // Multicast destination address
    uint16_t local_port = 0;             // Local port we bound to
};

// Encapsulates the Peel handshake logic
// Can be used as sender (1) or receiver (N)
class PeelHandshake {
public:
    explicit PeelHandshake(const PeelConfig& config);
    ~PeelHandshake();

    // Non-copyable
    PeelHandshake(const PeelHandshake&) = delete;
    PeelHandshake& operator=(const PeelHandshake&) = delete;

    // Initialize the UDP socket and multicast settings
    // Must be called before senderHandshake() or receiverHandshake()
    bool init();

    // Sender-side handshake:
    // 1. Multicast SYN
    // 2. Collect SYN+ACK from expected_receivers
    // 3. Multicast START
    // Returns cohort on success, nullopt on failure
    std::optional<PeelCohort> senderHandshake(int expected_receivers);

    // Receiver-side handshake:
    // 1. Wait for SYN
    // 2. Send SYN+ACK (unicast to sender)
    // 3. Wait for START
    // Returns cohort (containing sender info) on success, nullopt on failure
    std::optional<PeelCohort> receiverHandshake();

    // Get local port after init()
    uint16_t localPort() const;

    // Check if initialized
    bool isInitialized() const { return fd_ >= 0; }

private:
    // Send packet via multicast
    bool xmit(const std::vector<uint8_t>& bytes);

    // Send packet to specific destination (unicast)
    bool xmitTo(const std::vector<uint8_t>& bytes, const sockaddr_in& dest);

    // Receive a packet with timeout
    // Returns false on timeout or error
    bool recvPacket(sockaddr_in& from, PeelHeader& hdr,
                    std::vector<uint8_t>* payload = nullptr,
                    int timeout_ms = -1);

    // Set socket receive timeout
    void setRecvTimeout(int timeout_ms);

    // Header manipulation
    void fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags,
                    uint16_t wnd, uint32_t ts, uint32_t tsecr,
                    uint8_t retrans_id);
    void serializeHeader(PeelHeader& h, uint8_t* out);
    void deserializeHeader(const uint8_t* in, PeelHeader& h);
    bool verifyHeader(const PeelHeader& h);

    // Send ACK packet
    void sendAck(const sockaddr_in& to, uint32_t seq, uint16_t flags,
                 uint32_t tsecr, uint8_t retrans_id);

private:
    PeelConfig config_;
    int fd_ = -1;
    sockaddr_in mcast_{};
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo