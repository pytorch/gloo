#include "gloo/transport/tcp/peel/peel_protocol.h"

#include <chrono>
#include <cstring>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

uint32_t peel_now_ms() {
    auto now = Clock::now().time_since_epoch();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
    );
}

uint16_t peel_checksum16(const void* data, size_t len) {
    // Internet checksum (RFC 1071) over bytes
    uint32_t sum = 0;
    const uint16_t* p = static_cast<const uint16_t*>(data);

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        uint16_t last = 0;
        *reinterpret_cast<uint8_t*>(&last) = *reinterpret_cast<const uint8_t*>(p);
        sum += last;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

uint16_t peel_header_checksum(const PeelHeader& h) {
    PeelHeader tmp = h;
    tmp.checksum = 0;
    return peel_checksum16(&tmp, sizeof(tmp));
}

void peel_set_header_checksum(PeelHeader& h) {
    h.checksum = 0;
    h.checksum = peel_header_checksum(h);
}

bool peel_verify_header_checksum(const PeelHeader& h) {
    return h.checksum == peel_header_checksum(h);
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo