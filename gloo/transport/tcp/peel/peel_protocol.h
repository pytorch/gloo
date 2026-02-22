#pragma once

#include <cstddef>
#include <cstdint>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

#pragma pack(push, 1)
struct PeelHeader {
    uint32_t seq;        // Sequence Number
    uint16_t src_port;   // Sender's UDP port (network order)
    uint16_t flags;      // Bit flags (network order)
    uint8_t  retrans_id; // Retransmission attempt id (1..8)
    uint8_t  reserved;   // Must be zero
    uint16_t window;     // Window size (network order)
    uint32_t tsval;      // Sender timestamp (network order)
    uint32_t tsecr;      // Echoed timestamp (network order)
    uint16_t checksum;   // Internet checksum over header only
};
#pragma pack(pop)

static_assert(sizeof(PeelHeader) == 22, "PeelHeader must be 22 bytes");

// Flags
enum PeelFlags : uint16_t {
    FLG_SYN   = 0x0001,
    FLG_ACK   = 0x0002,
    FLG_START = 0x0004,
    FLG_DATA  = 0x0008,
    FLG_FIN   = 0x0010,
    FLG_RST   = 0x0020,
};

// Utilities
uint32_t peel_now_ms();
uint16_t peel_checksum16(const void* data, size_t len);

// Header checksum helpers
uint16_t peel_header_checksum(const PeelHeader& h);
void peel_set_header_checksum(PeelHeader& h);
bool peel_verify_header_checksum(const PeelHeader& h);

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo