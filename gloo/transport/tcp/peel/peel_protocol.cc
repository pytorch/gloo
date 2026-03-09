#include "peel_protocol.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>

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
    uint32_t sum = 0;
    const auto* p = static_cast<const uint16_t*>(data);

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

void peel_fill_header(PeelHeader& h, uint32_t seq, uint16_t flags,
                      uint16_t src_port, uint8_t rank, uint8_t retrans_id,
                      uint32_t tsval, uint32_t tsecr, uint16_t window) {
    std::memset(&h, 0, sizeof(h));
    h.seq = htonl(seq);
    h.src_port = htons(src_port);
    h.flags = htons(flags);
    h.retrans_id = retrans_id;
    h.rank = rank;
    h.window = htons(window);
    h.tsval = htonl(tsval == 0 ? peel_now_ms() : tsval);
    h.tsecr = htonl(tsecr);
    h.checksum = 0;
}

std::string peel_header_to_string(const PeelHeader& h) {
    std::ostringstream oss;
    uint16_t flags = ntohs(h.flags);
    oss << "Peel{seq=" << ntohl(h.seq)
        << " flags=";
    if (flags & FLG_SYN)   oss << "SYN|";
    if (flags & FLG_ACK)   oss << "ACK|";
    if (flags & FLG_START) oss << "START|";
    if (flags & FLG_DATA)  oss << "DATA|";
    if (flags & FLG_FIN)   oss << "FIN|";
    if (flags & FLG_RST)   oss << "RST|";
    if (flags & FLG_NACK)  oss << "NACK|";
    oss << " rank=" << (int)h.rank
        << " retrans=" << (int)h.retrans_id
        << " ts=" << ntohl(h.tsval)
        << "}";
    return oss.str();
}

std::string peel_addr_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo