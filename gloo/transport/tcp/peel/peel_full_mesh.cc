#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

// ============================================================================
// PeelChannel Implementation
// ============================================================================

PeelChannel::~PeelChannel() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

PeelChannel::PeelChannel(PeelChannel&& other) noexcept
    : fd(other.fd)
    , sender_rank(other.sender_rank)
    , port(other.port)
    , mcast(other.mcast)
    , is_send_channel(other.is_send_channel) {
    other.fd = -1;
}

PeelChannel& PeelChannel::operator=(PeelChannel&& other) noexcept {
    if (this != &other) {
        if (fd >= 0) ::close(fd);
        fd = other.fd;
        sender_rank = other.sender_rank;
        port = other.port;
        mcast = other.mcast;
        is_send_channel = other.is_send_channel;
        other.fd = -1;
    }
    return *this;
}

int PeelChannel::releaseFd() {
    int ret = fd;
    fd = -1;
    return ret;
}

// ============================================================================
// PeelFullMeshResult Implementation
// ============================================================================

PeelChannel* PeelFullMeshResult::getRecvChannel(int sender_rank) {
    for (auto& ch : recv_channels) {
        if (ch && ch->sender_rank == sender_rank) {
            return ch.get();
        }
    }
    return nullptr;
}

// ============================================================================
// PeelFullMesh Implementation
// ============================================================================

PeelFullMesh::PeelFullMesh(const PeelFullMeshConfig& config)
    : config_(config) {
    recv_fds_.resize(config_.world_size, -1);
}

PeelFullMesh::~PeelFullMesh() {
    if (send_fd_ >= 0) {
        ::close(send_fd_);
        send_fd_ = -1;
    }
    for (auto& fd : recv_fds_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

bool PeelFullMesh::init() {
    std::cerr << "peel: rank " << config_.rank << " initializing...\n";

    // Step 1: Connect to Redis
    if (!connectRedis()) {
        std::cerr << "peel: rank " << config_.rank << " failed to connect to Redis\n";
        return false;
    }

    // Step 2: Create all sockets
    if (!createSockets()) {
        std::cerr << "peel: rank " << config_.rank << " failed to create sockets\n";
        return false;
    }

    // Step 3: Signal ready to Redis
    if (!signalReady()) {
        std::cerr << "peel: rank " << config_.rank << " failed to signal ready\n";
        return false;
    }

    std::cerr << "peel: rank " << config_.rank << " initialized successfully\n";
    return true;
}

std::unique_ptr<PeelFullMeshResult> PeelFullMesh::run() {
    std::cerr << "peel: rank " << config_.rank << " starting full-mesh handshake...\n";

    // Step 1: Wait for all ranks to be ready (Redis barrier)
    if (!waitForAllReady()) {
        std::cerr << "peel: rank " << config_.rank << " barrier timeout\n";
        return nullptr;
    }

    std::cerr << "peel: rank " << config_.rank << " all ranks ready, starting handshake\n";

    // Step 2: Perform handshake
    auto result = std::make_unique<PeelFullMeshResult>();
    result->rank = config_.rank;
    result->world_size = config_.world_size;
    result->peers.resize(config_.world_size);

    if (!performHandshake(*result)) {
        std::cerr << "peel: rank " << config_.rank << " handshake failed\n";
        return nullptr;
    }

    // Step 3: Transfer socket ownership to result
    result->send_channel = std::make_unique<PeelChannel>();
    result->send_channel->fd = send_fd_;
    result->send_channel->sender_rank = config_.rank;
    result->send_channel->port = config_.sendPort();
    result->send_channel->mcast = mcast_base_;
    result->send_channel->mcast.sin_port = htons(config_.sendPort());
    result->send_channel->is_send_channel = true;
    send_fd_ = -1;  // Transferred

    for (int r = 0; r < config_.world_size; ++r) {
        if (r == config_.rank) continue;
        if (recv_fds_[r] < 0) continue;

        auto channel = std::make_unique<PeelChannel>();
        channel->fd = recv_fds_[r];
        channel->sender_rank = r;
        channel->port = config_.recvPort(r);
        channel->mcast = mcast_base_;
        channel->mcast.sin_port = htons(config_.recvPort(r));
        channel->is_send_channel = false;
        recv_fds_[r] = -1;  // Transferred

        result->recv_channels.push_back(std::move(channel));
    }

    std::cerr << "peel: rank " << config_.rank << " handshake complete!\n";
    return result;
}

void PeelFullMesh::cleanup() {
    if (redis_) {
        std::string pattern = config_.redis_prefix + "/*";
        int deleted = redis_->delPattern(pattern);
        std::cerr << "peel: cleaned up " << deleted << " Redis keys\n";
    }
}

// ============================================================================
// Internal Steps
// ============================================================================

bool PeelFullMesh::connectRedis() {
    redis_ = std::make_unique<PeelRedis>(config_.redis_host, config_.redis_port);
    return redis_->connect();
}

bool PeelFullMesh::createSockets() {
    // Parse multicast group address
    std::memset(&mcast_base_, 0, sizeof(mcast_base_));
    mcast_base_.sin_family = AF_INET;
    if (inet_pton(AF_INET, config_.mcast_group.c_str(), &mcast_base_.sin_addr) != 1) {
        std::cerr << "peel: invalid multicast group: " << config_.mcast_group << "\n";
        return false;
    }

    // Create send socket (this rank's channel)
    uint16_t send_port = config_.sendPort();
    send_fd_ = createSocket(send_port, true);
    if (send_fd_ < 0) {
        std::cerr << "peel: failed to create send socket on port " << send_port << "\n";
        return false;
    }
    std::cerr << "peel: rank " << config_.rank << " created send socket on port " << send_port << "\n";

    // Create receive sockets (other ranks' channels)
    for (int r = 0; r < config_.world_size; ++r) {
        if (r == config_.rank) continue;

        uint16_t recv_port = config_.recvPort(r);
        recv_fds_[r] = createSocket(recv_port, false);
        if (recv_fds_[r] < 0) {
            std::cerr << "peel: failed to create recv socket for rank " << r
                      << " on port " << recv_port << "\n";
            return false;
        }
        std::cerr << "peel: rank " << config_.rank << " created recv socket on port "
                  << recv_port << " (for rank " << r << ")\n";
    }

    return true;
}

bool PeelFullMesh::signalReady() {
    std::string key = config_.redis_prefix + "/ready/" + std::to_string(config_.rank);
    if (!redis_->set(key, "1")) {
        std::cerr << "peel: failed to set ready key: " << key << "\n";
        return false;
    }
    std::cerr << "peel: rank " << config_.rank << " signaled ready\n";
    return true;
}

bool PeelFullMesh::waitForAllReady() {
    std::cerr << "peel: rank " << config_.rank << " waiting for all ranks...\n";

    std::vector<std::string> keys;
    for (int r = 0; r < config_.world_size; ++r) {
        keys.push_back(config_.redis_prefix + "/ready/" + std::to_string(r));
    }

    bool ok = redis_->waitForKeys(keys, config_.handshake_timeout_ms, config_.poll_interval_ms);
    
    if (ok) {
        std::cerr << "peel: rank " << config_.rank << " all " << config_.world_size << " ranks ready\n";
    }
    
    return ok;
}

bool PeelFullMesh::performHandshake(PeelFullMeshResult& result) {
    auto start_time = Clock::now();
    auto timeout = std::chrono::milliseconds(config_.handshake_timeout_ms);

    // Track which peers we've heard from
    std::vector<bool> heard_syn(config_.world_size, false);
    std::vector<bool> heard_ack(config_.world_size, false);
    heard_syn[config_.rank] = true;  // Don't need to hear from self
    heard_ack[config_.rank] = true;
    int syn_count = 1;
    int ack_count = 1;

    // Prepare multicast destination for sending
    sockaddr_in send_dest = mcast_base_;
    send_dest.sin_port = htons(config_.sendPort());

    int attempt = 0;
    constexpr int kMaxAttempts = 20;

    while ((syn_count < config_.world_size || ack_count < config_.world_size) &&
           Clock::now() - start_time < timeout &&
           attempt < kMaxAttempts) {

        ++attempt;

        // Send SYN on our channel
        PeelHeader syn_hdr{};
        fillHeader(syn_hdr, 0, FLG_SYN, static_cast<uint8_t>(attempt));
        sendPacket(send_fd_, send_dest, syn_hdr);

        std::cerr << "peel: rank " << config_.rank << " sent SYN (attempt " << attempt
                  << "), heard SYN from " << syn_count << "/" << config_.world_size
                  << ", ACK from " << ack_count << "/" << config_.world_size << "\n";

        // Listen on all receive channels for a short time (RTO)
        auto rto_start = Clock::now();
        auto rto_duration = std::chrono::milliseconds(config_.rto_ms);

        while (Clock::now() - rto_start < rto_duration) {
            // Check all receive sockets using select
            fd_set readfds;
            FD_ZERO(&readfds);
            int max_fd = -1;

            for (int r = 0; r < config_.world_size; ++r) {
                if (r == config_.rank) continue;
                if (recv_fds_[r] >= 0) {
                    FD_SET(recv_fds_[r], &readfds);
                    if (recv_fds_[r] > max_fd) max_fd = recv_fds_[r];
                }
            }

            if (max_fd < 0) break;

            // Short timeout for select
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 10000;  // 10ms

            int ready = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            // Check each socket
            for (int r = 0; r < config_.world_size; ++r) {
                if (r == config_.rank) continue;
                int fd = recv_fds_[r];
                if (fd < 0 || !FD_ISSET(fd, &readfds)) continue;

                // Receive packet
                sockaddr_in from{};
                socklen_t fromlen = sizeof(from);
                uint8_t buf[sizeof(PeelHeader) + 64];

                ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&from), &fromlen);
                if (n < static_cast<ssize_t>(sizeof(PeelHeader))) continue;

                PeelHeader recv_hdr{};
                std::memcpy(&recv_hdr, buf, sizeof(recv_hdr));

                if (!peel_verify_header_checksum(recv_hdr)) continue;

                uint16_t flags = ntohs(recv_hdr.flags);
                uint16_t src_rank = ntohs(recv_hdr.src_port);

                // Verify src_rank matches expected sender for this channel
                if (src_rank != r) continue;

                // Process SYN
                if ((flags & FLG_SYN) && !heard_syn[r]) {
                    heard_syn[r] = true;
                    ++syn_count;
                    result.peers[r] = from;

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                    std::cerr << "peel: rank " << config_.rank << " received SYN from rank "
                              << r << " (" << ip_str << ")\n";

                    // Send ACK on our channel
                    PeelHeader ack_hdr{};
                    fillHeader(ack_hdr, 0, FLG_SYN | FLG_ACK, recv_hdr.retrans_id);
                    ack_hdr.tsecr = recv_hdr.tsval;
                    peel_set_header_checksum(ack_hdr);
                    sendPacket(send_fd_, send_dest, ack_hdr);

                    std::cerr << "peel: rank " << config_.rank << " sent ACK to rank " << r << "\n";
                }

                // Process ACK (response to our SYN)
                if ((flags & FLG_ACK) && !heard_ack[r]) {
                    heard_ack[r] = true;
                    ++ack_count;
                    std::cerr << "peel: rank " << config_.rank << " received ACK from rank " << r << "\n";
                }
            }
        }
    }

    if (syn_count < config_.world_size) {
        std::cerr << "peel: rank " << config_.rank << " timeout: only heard SYN from "
                  << syn_count << "/" << config_.world_size << " ranks\n";
        return false;
    }

    if (ack_count < config_.world_size) {
        std::cerr << "peel: rank " << config_.rank << " timeout: only heard ACK from "
                  << ack_count << "/" << config_.world_size << " ranks\n";
        // This is a warning, not fatal - we heard from everyone, they may not have ACKed yet
    }

    // Send START to signal completion
    PeelHeader start_hdr{};
    fillHeader(start_hdr, 0, FLG_START, 0);
    sendPacket(send_fd_, send_dest, start_hdr);
    std::cerr << "peel: rank " << config_.rank << " sent START\n";

    return true;
}

// ============================================================================
// Socket Helpers
// ============================================================================

int PeelFullMesh::createSocket(uint16_t port, bool is_sender) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // Allow address reuse (multiple processes on same host)
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        ::close(fd);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) < 0) {
        // Non-fatal on some systems
        perror("setsockopt SO_REUSEPORT (non-fatal)");
    }

    // Bind to port
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        perror("bind");
        ::close(fd);
        return -1;
    }

    // Join multicast group
    ip_mreq mreq{};
    mreq.imr_multiaddr = mcast_base_.sin_addr;
    if (!config_.iface_ip.empty()) {
        if (inet_pton(AF_INET, config_.iface_ip.c_str(), &mreq.imr_interface) != 1) {
            std::cerr << "peel: invalid interface IP: " << config_.iface_ip << "\n";
            ::close(fd);
            return -1;
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        ::close(fd);
        return -1;
    }

    // Sender-specific settings
    if (is_sender) {
        // Set multicast interface
        if (!config_.iface_ip.empty()) {
            in_addr ifaddr{};
            inet_pton(AF_INET, config_.iface_ip.c_str(), &ifaddr);
            if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
                perror("setsockopt IP_MULTICAST_IF");
                ::close(fd);
                return -1;
            }
        }

        // Set TTL
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                       &config_.ttl, sizeof(config_.ttl)) < 0) {
            perror("setsockopt IP_MULTICAST_TTL");
            ::close(fd);
            return -1;
        }

        // Disable loopback (don't receive own packets)
        int loop = 0;
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
            perror("setsockopt IP_MULTICAST_LOOP (non-fatal)");
        }
    }

    // Set receive buffer size
    if (config_.rcvbuf > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                       &config_.rcvbuf, sizeof(config_.rcvbuf)) < 0) {
            perror("setsockopt SO_RCVBUF (non-fatal)");
        }
    }

    return fd;
}

void PeelFullMesh::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void PeelFullMesh::setRecvTimeout(int fd, int timeout_ms) {
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// ============================================================================
// Packet Helpers
// ============================================================================

void PeelFullMesh::fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags, uint8_t retrans_id) {
    std::memset(&h, 0, sizeof(h));
    h.seq = htonl(seq);
    h.src_port = htons(static_cast<uint16_t>(config_.rank));  // Use rank as identifier
    h.flags = htons(flags);
    h.retrans_id = retrans_id;
    h.reserved = 0;
    h.window = htons(1);
    h.tsval = htonl(peel_now_ms());
    h.tsecr = 0;
    peel_set_header_checksum(h);
}

bool PeelFullMesh::sendPacket(int fd, const sockaddr_in& dest, const PeelHeader& hdr) {
    ssize_t n = sendto(fd, &hdr, sizeof(hdr), 0,
                       reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    if (n < 0) {
        perror("sendto");
        return false;
    }
    return n == sizeof(hdr);
}

ssize_t PeelFullMesh::recvPacketNonBlocking(int fd, sockaddr_in& from, PeelHeader& hdr, int timeout_ms) {
    // Use select for timeout
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ready = select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) {
        return 0;  // Timeout or error
    }

    socklen_t fromlen = sizeof(from);
    uint8_t buf[sizeof(PeelHeader) + 64];

    ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n >= static_cast<ssize_t>(sizeof(PeelHeader))) {
        std::memcpy(&hdr, buf, sizeof(hdr));
    }
    return n;
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo