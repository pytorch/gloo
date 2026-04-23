#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// =============================================================================
// Constants
// =============================================================================

constexpr uint16_t PEEL_DEFAULT_BASE_PORT = 50000;
constexpr uint16_t PEEL_HEADER_SIZE = 22;
constexpr size_t PEEL_MAX_PAYLOAD = 8900;  // Safe for most MTUs
constexpr int PEEL_DEFAULT_TTL = 64;
constexpr int PEEL_DEFAULT_RTO_MS = 500;
constexpr int PEEL_DEFAULT_RETRIES = 20;
constexpr int PEEL_DEFAULT_TIMEOUT_MS = 300000;

// =============================================================================
// Header (22 bytes, all multi-byte fields in network order)
// =============================================================================

#pragma pack(push, 1)
struct PeelHeader {
    uint32_t seq;        // Sequence number
    uint16_t src_port;   // Sender's port (for ACK routing)
    uint16_t flags;      // Protocol flags
    uint8_t  retrans_id; // Retransmission attempt (1-255)
    uint8_t  rank;       // Sender's rank
    uint16_t window;     // Flow control window
    uint32_t tsval;      // Sender timestamp
    uint32_t tsecr;      // Echoed timestamp (for RTT)
    uint16_t checksum;   // Internet checksum over header
};
#pragma pack(pop)

static_assert(sizeof(PeelHeader) == PEEL_HEADER_SIZE, "PeelHeader must be 22 bytes");

// =============================================================================
// Flags
// =============================================================================

enum PeelFlags : uint16_t {
    FLG_NONE  = 0x0000,
    FLG_SYN   = 0x0001,  // Handshake initiation
    FLG_ACK   = 0x0002,  // Acknowledgment
    FLG_START = 0x0004,  // Data phase begin
    FLG_DATA  = 0x0008,  // Data packet
    FLG_FIN   = 0x0010,  // Transfer complete
    FLG_RST   = 0x0020,  // Reset/abort
    FLG_NACK  = 0x0040,  // Negative ack (retransmit request)
};

// =============================================================================
// Utility Functions
// =============================================================================

uint32_t peel_now_ms();
uint16_t peel_checksum16(const void* data, size_t len);

uint16_t peel_header_checksum(const PeelHeader& h);
void peel_set_header_checksum(PeelHeader& h);
bool peel_verify_header_checksum(const PeelHeader& h);

void peel_fill_header(PeelHeader& h, uint32_t seq, uint16_t flags,
                      uint16_t src_port, uint8_t rank, uint8_t retrans_id = 1,
                      uint32_t tsval = 0, uint32_t tsecr = 0, uint16_t window = 1);

std::string peel_header_to_string(const PeelHeader& h);
std::string peel_addr_to_string(const sockaddr_in& addr);

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo