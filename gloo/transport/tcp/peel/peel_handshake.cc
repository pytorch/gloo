#include "gloo/transport/tcp/peel/peel_handshake.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

namespace {

// Key for identifying unique peers
struct PeerKey {
    uint32_t ip;   // network order
    uint16_t port; // network order
    bool operator==(const PeerKey& o) const {
        return ip == o.ip && port == o.port;
    }
};

struct PeerKeyHash {
    size_t operator()(const PeerKey& k) const {
        return static_cast<size_t>(k.ip) * 1315423911u + k.port;
    }
};

} // anonymous namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

PeelHandshake::PeelHandshake(const PeelConfig& config)
    : config_(config) {
}

PeelHandshake::~PeelHandshake() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ============================================================================
// Initialization
// ============================================================================

bool PeelHandshake::init() {
    // Create UDP socket
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        std::cerr << "peel: socket() failed: " << strerror(errno) << "\n";
        return false;
    }

    // Allow address reuse
    int yes = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        std::cerr << "peel: SO_REUSEADDR failed: " << strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Bind to local port
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(config_.local_port == 0 ? config_.mcast_port : config_.local_port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        std::cerr << "peel: bind() failed: " << strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Setup multicast destination address
    std::memset(&mcast_, 0, sizeof(mcast_));
    mcast_.sin_family = AF_INET;
    mcast_.sin_port = htons(config_.mcast_port);
    if (inet_pton(AF_INET, config_.group.c_str(), &mcast_.sin_addr) != 1) {
        std::cerr << "peel: invalid multicast group: " << config_.group << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Set multicast interface (for sending)
    if (!config_.iface_ip.empty()) {
        in_addr ifaceAddr{};
        if (inet_pton(AF_INET, config_.iface_ip.c_str(), &ifaceAddr) == 1) {
            if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF,
                           &ifaceAddr, sizeof(ifaceAddr)) < 0) {
                std::cerr << "peel: IP_MULTICAST_IF failed: "
                          << strerror(errno) << "\n";
                // Non-fatal, continue
            }
        }
    }

    // Set multicast TTL
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                   &config_.ttl, sizeof(config_.ttl)) < 0) {
        std::cerr << "peel: IP_MULTICAST_TTL failed: " << strerror(errno) << "\n";
        // Non-fatal
    }

    // Disable multicast loopback (don't receive our own packets)
    int loop = 0;
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                   &loop, sizeof(loop)) < 0) {
        std::cerr << "peel: IP_MULTICAST_LOOP failed: " << strerror(errno) << "\n";
        // Non-fatal
    }

    // Join multicast group (for receiving)
    ip_mreq mreq{};
    mreq.imr_multiaddr = mcast_.sin_addr;
    if (!config_.iface_ip.empty()) {
        if (inet_pton(AF_INET, config_.iface_ip.c_str(),
                      &mreq.imr_interface) != 1) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        std::cerr << "peel: IP_ADD_MEMBERSHIP failed: " << strerror(errno) << "\n";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    std::cerr << "peel: initialized on port " << localPort()
              << ", multicast group " << config_.group
              << ":" << config_.mcast_port << "\n";

    return true;
}

// ============================================================================
// Sender Handshake
// ============================================================================

std::optional<PeelCohort> PeelHandshake::senderHandshake(int expected_receivers) {
    if (fd_ < 0) {
        std::cerr << "peel: not initialized\n";
        return std::nullopt;
    }

    if (expected_receivers <= 0) {
        std::cerr << "peel: expected_receivers must be > 0\n";
        return std::nullopt;
    }

    auto handshake_start = Clock::now();
    auto elapsed_ms = [&]() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - handshake_start).count();
    };

    std::unordered_map<PeerKey, sockaddr_in, PeerKeyHash> cohort_map;
    constexpr uint8_t kMaxRetransId = 8;
    bool success = false;

    for (int attempt = 0, retrans_id = 1;
         attempt <= config_.retries && retrans_id <= kMaxRetransId;
         ++attempt, ++retrans_id) {

        // Clear cohort on retry (start fresh)
        if (attempt > 0) {
            cohort_map.clear();
        }

        // Build and send SYN
        uint32_t ts = peel_now_ms();
        PeelHeader h{};
        fillHeader(h, /*seq=*/0, FLG_SYN, /*wnd=*/1, ts, /*tsecr=*/0,
                   static_cast<uint8_t>(retrans_id));

        std::vector<uint8_t> pkt(sizeof(PeelHeader));
        serializeHeader(h, pkt.data());

        if (!xmit(pkt)) {
            std::cerr << "peel: failed to send SYN (attempt " << (attempt + 1) << ")\n";
            continue;
        }

        std::cerr << "peel: SYN sent (attempt " << (attempt + 1)
                  << ", retrans_id=" << retrans_id << ")\n";

        // Wait for SYN+ACKs within RTO window
        auto deadline = Clock::now() + std::chrono::milliseconds(config_.rto_ms);

        while (Clock::now() < deadline) {
            int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now()).count());
            if (remaining_ms <= 0) break;

            sockaddr_in peer{};
            PeelHeader rh{};
            if (!recvPacket(peer, rh, nullptr, remaining_ms)) {
                break;  // Timeout
            }

            // Validate response
            if (!verifyHeader(rh)) {
                continue;
            }
            uint16_t flags = ntohs(rh.flags);
            if ((flags & (FLG_SYN | FLG_ACK)) != (FLG_SYN | FLG_ACK)) {
                continue;
            }
            if (rh.retrans_id != static_cast<uint8_t>(retrans_id)) {
                continue;
            }
            if (ntohl(rh.tsecr) != ts) {
                continue;
            }

            // Valid SYN+ACK - add to cohort
            PeerKey k{peer.sin_addr.s_addr, peer.sin_port};
            if (cohort_map.find(k) == cohort_map.end()) {
                cohort_map[k] = peer;
                std::cerr << "peel: SYN+ACK from " << inet_ntoa(peer.sin_addr)
                          << ":" << ntohs(peer.sin_port)
                          << " (" << cohort_map.size() << "/" << expected_receivers << ")\n";
            }

            if (static_cast<int>(cohort_map.size()) >= expected_receivers) {
                break;
            }
        }

        if (static_cast<int>(cohort_map.size()) >= expected_receivers) {
            success = true;
            break;
        }

        std::cerr << "peel: timeout, got " << cohort_map.size()
                  << "/" << expected_receivers << " -> retrying\n";
    }

    if (!success) {
        std::cerr << "peel: handshake failed after " << elapsed_ms() << " ms, got "
                  << cohort_map.size() << "/" << expected_receivers << " receivers\n";
        return std::nullopt;
    }

    // Send START to notify receivers
    {
        uint32_t ts = peel_now_ms();
        PeelHeader start{};
        fillHeader(start, /*seq=*/0, FLG_START, /*wnd=*/1, ts, /*tsecr=*/0,
                   /*retrans_id=*/1);

        std::vector<uint8_t> pkt(sizeof(PeelHeader));
        serializeHeader(start, pkt.data());

        if (!xmit(pkt)) {
            std::cerr << "peel: failed to send START\n";
            return std::nullopt;
        }

        std::cerr << "peel: START sent\n";
    }

    // Build result
    PeelCohort result;
    result.fd = fd_;
    result.mcast = mcast_;
    result.local_port = localPort();
    result.peers.reserve(cohort_map.size());
    for (auto& kv : cohort_map) {
        result.peers.push_back(kv.second);
    }

    std::cerr << "peel: sender handshake complete in " << elapsed_ms()
              << " ms, cohort size=" << result.peers.size() << "\n";

    // Transfer ownership of socket
    fd_ = -1;
    return result;
}

// ============================================================================
// Receiver Handshake
// ============================================================================

std::optional<PeelCohort> PeelHandshake::receiverHandshake() {
    if (fd_ < 0) {
        std::cerr << "peel: not initialized\n";
        return std::nullopt;
    }

    auto handshake_start = Clock::now();
    auto elapsed_ms = [&]() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - handshake_start).count();
    };

    auto deadline = Clock::now() +
        std::chrono::milliseconds(config_.handshake_timeout_ms);

    sockaddr_in sender{};
    bool got_syn = false;
    bool got_start = false;

    std::cerr << "peel: receiver waiting for SYN...\n";

    while (Clock::now() < deadline) {
        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        if (remaining_ms <= 0) break;

        sockaddr_in peer{};
        PeelHeader h{};
        if (!recvPacket(peer, h, nullptr, remaining_ms)) {
            continue;  // Timeout or error, keep waiting
        }

        if (!verifyHeader(h)) {
            continue;
        }

        uint16_t flags = ntohs(h.flags);
        uint32_t tsval = ntohl(h.tsval);
        uint16_t sender_port = ntohs(h.src_port);
        uint8_t retrans_id = h.retrans_id;

        // Build ACK destination (use src_port from header)
        sockaddr_in ack_to{};
        ack_to.sin_family = AF_INET;
        ack_to.sin_addr = peer.sin_addr;
        ack_to.sin_port = htons(sender_port);

        if (flags & FLG_SYN) {
            // Respond with SYN+ACK
            sendAck(ack_to, /*seq=*/0, FLG_SYN | FLG_ACK, tsval, retrans_id);

            sender = ack_to;
            got_syn = true;

            std::cerr << "peel: SYN from " << inet_ntoa(peer.sin_addr)
                      << ":" << sender_port
                      << " -> sent SYN+ACK (retrans_id=" << (int)retrans_id << ")\n";
            continue;
        }

        if (flags & FLG_START) {
            if (!got_syn) {
                std::cerr << "peel: got START before SYN, ignoring\n";
                continue;
            }
            got_start = true;
            std::cerr << "peel: START received\n";
            break;
        }
    }

    if (!got_start) {
        std::cerr << "peel: receiver handshake timeout after " << elapsed_ms()
                  << " ms (got_syn=" << got_syn << ")\n";
        return std::nullopt;
    }

    // Build result
    PeelCohort result;
    result.fd = fd_;
    result.mcast = mcast_;
    result.local_port = localPort();
    result.peers.push_back(sender);

    std::cerr << "peel: receiver handshake complete in " << elapsed_ms() << " ms\n";

    // Transfer ownership of socket
    fd_ = -1;
    return result;
}

// ============================================================================
// Utility Methods
// ============================================================================

uint16_t PeelHandshake::localPort() const {
    if (fd_ < 0) return 0;

    sockaddr_in sa{};
    socklen_t sl = sizeof(sa);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &sl) == 0) {
        return ntohs(sa.sin_port);
    }
    return 0;
}

bool PeelHandshake::xmit(const std::vector<uint8_t>& bytes) {
    ssize_t n = sendto(fd_, bytes.data(), bytes.size(), 0,
                       reinterpret_cast<const sockaddr*>(&mcast_),
                       sizeof(mcast_));
    if (n < 0) {
        std::cerr << "peel: sendto() failed: " << strerror(errno) << "\n";
        return false;
    }
    if (static_cast<size_t>(n) != bytes.size()) {
        std::cerr << "peel: partial send: " << n << "/" << bytes.size() << "\n";
        return false;
    }
    return true;
}

bool PeelHandshake::xmitTo(const std::vector<uint8_t>& bytes,
                           const sockaddr_in& dest) {
    ssize_t n = sendto(fd_, bytes.data(), bytes.size(), 0,
                       reinterpret_cast<const sockaddr*>(&dest),
                       sizeof(dest));
    if (n < 0) {
        std::cerr << "peel: sendto() failed: " << strerror(errno) << "\n";
        return false;
    }
    return static_cast<size_t>(n) == bytes.size();
}

bool PeelHandshake::recvPacket(sockaddr_in& from, PeelHeader& hdr,
                               std::vector<uint8_t>* payload,
                               int timeout_ms) {
    setRecvTimeout(timeout_ms);

    uint8_t buf[65536];
    socklen_t alen = sizeof(from);

    ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &alen);
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
            return false;  // Timeout
        }
        std::cerr << "peel: recvfrom() failed: " << strerror(errno) << "\n";
        return false;
    }

    if (static_cast<size_t>(n) < sizeof(PeelHeader)) {
        return false;  // Packet too small
    }

    deserializeHeader(buf, hdr);

    if (payload != nullptr && static_cast<size_t>(n) > sizeof(PeelHeader)) {
        size_t payload_len = static_cast<size_t>(n) - sizeof(PeelHeader);
        payload->assign(buf + sizeof(PeelHeader),
                        buf + sizeof(PeelHeader) + payload_len);
    }

    return true;
}

void PeelHandshake::setRecvTimeout(int timeout_ms) {
    timeval tv{};
    if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    } else {
        // No timeout (blocking)
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void PeelHandshake::fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags,
                               uint16_t wnd, uint32_t ts, uint32_t tsecr,
                               uint8_t retrans_id) {
    h.seq = htonl(seq);
    h.src_port = htons(localPort());
    h.flags = htons(flags);
    h.retrans_id = retrans_id;
    h.reserved = 0;
    h.window = htons(wnd);
    h.tsval = htonl(ts);
    h.tsecr = htonl(tsecr);
    h.checksum = 0;
}

void PeelHandshake::serializeHeader(PeelHeader& h, uint8_t* out) {
    peel_set_header_checksum(h);
    std::memcpy(out, &h, sizeof(h));
}

void PeelHandshake::deserializeHeader(const uint8_t* in, PeelHeader& h) {
    std::memcpy(&h, in, sizeof(h));
}

bool PeelHandshake::verifyHeader(const PeelHeader& h) {
    return peel_verify_header_checksum(h);
}

void PeelHandshake::sendAck(const sockaddr_in& to, uint32_t seq, uint16_t flags,
                            uint32_t tsecr, uint8_t retrans_id) {
    PeelHeader ack{};
    fillHeader(ack, seq, flags, /*wnd=*/1, peel_now_ms(), tsecr, retrans_id);

    std::vector<uint8_t> pkt(sizeof(PeelHeader));
    serializeHeader(ack, pkt.data());

    xmitTo(pkt, to);
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo