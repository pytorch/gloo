#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
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

// =============================================================================
// PeelChannel
// =============================================================================

PeelChannel::~PeelChannel() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

PeelChannel::PeelChannel(PeelChannel&& o) noexcept
    : fd(o.fd), owner_rank(o.owner_rank), port(o.port),
      mcast(o.mcast), is_sender(o.is_sender) {
    o.fd = -1;
}

PeelChannel& PeelChannel::operator=(PeelChannel&& o) noexcept {
    if (this != &o) {
        if (fd >= 0) ::close(fd);
        fd = o.fd;
        owner_rank = o.owner_rank;
        port = o.port;
        mcast = o.mcast;
        is_sender = o.is_sender;
        o.fd = -1;
    }
    return *this;
}

int PeelChannel::releaseFd() {
    int ret = fd;
    fd = -1;
    return ret;
}

// =============================================================================
// PeelFullMeshResult
// =============================================================================

PeelChannel* PeelFullMeshResult::getRecvChannel(int sender_rank) {
    for (auto& ch : recv_channels) {
        if (ch && ch->owner_rank == sender_rank) return ch.get();
    }
    return nullptr;
}

const PeelChannel* PeelFullMeshResult::getRecvChannel(int sender_rank) const {
    for (const auto& ch : recv_channels) {
        if (ch && ch->owner_rank == sender_rank) return ch.get();
    }
    return nullptr;
}

// =============================================================================
// PeelFullMesh
// =============================================================================

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
    std::cerr << "peel[" << config_.rank << "]: initializing...\n";

    if (!connectRedis()) {
        std::cerr << "peel[" << config_.rank << "]: redis connect failed\n";
        return false;
    }

    if (!createSockets()) {
        std::cerr << "peel[" << config_.rank << "]: socket creation failed\n";
        return false;
    }

    if (!signalReady()) {
        std::cerr << "peel[" << config_.rank << "]: signal ready failed\n";
        return false;
    }

    std::cerr << "peel[" << config_.rank << "]: initialized\n";
    return true;
}

std::unique_ptr<PeelFullMeshResult> PeelFullMesh::run() {
    std::cerr << "peel[" << config_.rank << "]: starting handshake...\n";

    if (!waitForAllReady()) {
        std::cerr << "peel[" << config_.rank << "]: barrier timeout\n";
        return nullptr;
    }

    std::cerr << "peel[" << config_.rank << "]: all ranks ready\n";

    auto result = std::make_unique<PeelFullMeshResult>();
    result->rank = config_.rank;
    result->world_size = config_.world_size;
    result->peers.resize(config_.world_size);

    if (!performHandshake(*result)) {
        std::cerr << "peel[" << config_.rank << "]: handshake failed\n";
        return nullptr;
    }

    // Transfer send channel
    result->send_channel = std::make_unique<PeelChannel>();
    result->send_channel->fd = send_fd_;
    result->send_channel->owner_rank = config_.rank;
    result->send_channel->port = config_.sendPort();
    result->send_channel->mcast = mcast_base_;
    result->send_channel->mcast.sin_port = htons(config_.sendPort());
    result->send_channel->is_sender = true;
    send_fd_ = -1;

    // Transfer receive channels
    for (int r = 0; r < config_.world_size; ++r) {
        if (r == config_.rank || recv_fds_[r] < 0) continue;

        auto ch = std::make_unique<PeelChannel>();
        ch->fd = recv_fds_[r];
        ch->owner_rank = r;
        ch->port = config_.recvPort(r);
        ch->mcast = mcast_base_;
        ch->mcast.sin_port = htons(config_.recvPort(r));
        ch->is_sender = false;
        recv_fds_[r] = -1;

        result->recv_channels.push_back(std::move(ch));
    }

    std::cerr << "peel[" << config_.rank << "]: handshake complete\n";
    return result;
}

void PeelFullMesh::cleanup() {
    if (redis_) {
        std::string pattern = config_.redis_prefix + "/*";
        int n = redis_->delPattern(pattern);
        std::cerr << "peel: cleaned " << n << " redis keys\n";
    }
}

// =============================================================================
// Internal
// =============================================================================

bool PeelFullMesh::connectRedis() {
    redis_ = std::make_unique<PeelRedis>(config_.redis_host, config_.redis_port);
    return redis_->connect();
}

bool PeelFullMesh::createSockets() {
    std::memset(&mcast_base_, 0, sizeof(mcast_base_));
    mcast_base_.sin_family = AF_INET;
    if (inet_pton(AF_INET, config_.mcast_group.c_str(), &mcast_base_.sin_addr) != 1) {
        std::cerr << "peel: invalid mcast group: " << config_.mcast_group << "\n";
        return false;
    }

    // Send socket
    send_fd_ = createSocket(config_.sendPort(), true);
    if (send_fd_ < 0) {
        std::cerr << "peel: send socket failed on port " << config_.sendPort() << "\n";
        return false;
    }

    // Receive sockets
    for (int r = 0; r < config_.world_size; ++r) {
        if (r == config_.rank) continue;

        recv_fds_[r] = createSocket(config_.recvPort(r), false);
        if (recv_fds_[r] < 0) {
            std::cerr << "peel: recv socket failed for rank " << r << "\n";
            return false;
        }
    }

    return true;
}

bool PeelFullMesh::signalReady() {
    std::string key = config_.redis_prefix + "/ready/" + std::to_string(config_.rank);
    return redis_->set(key, "1");
}

bool PeelFullMesh::waitForAllReady() {
    std::vector<std::string> keys;
    for (int r = 0; r < config_.world_size; ++r) {
        keys.push_back(config_.redis_prefix + "/ready/" + std::to_string(r));
    }
    return redis_->waitForKeys(keys, config_.handshake_timeout_ms, config_.poll_interval_ms);
}

bool PeelFullMesh::performHandshake(PeelFullMeshResult& result) {
    auto start = Clock::now();
    auto timeout = std::chrono::milliseconds(config_.handshake_timeout_ms);

    std::vector<bool> heard_syn(config_.world_size, false);
    std::vector<bool> heard_ack(config_.world_size, false);
    heard_syn[config_.rank] = true;
    heard_ack[config_.rank] = true;
    int syn_count = 1;
    int ack_count = 1;

    sockaddr_in send_dest = mcast_base_;
    send_dest.sin_port = htons(config_.sendPort());

    constexpr int kMaxAttempts = 20;

    for (int attempt = 1;
         (syn_count < config_.world_size || ack_count < config_.world_size) &&
         Clock::now() - start < timeout && attempt <= kMaxAttempts;
         ++attempt) {

        // Send SYN
        PeelHeader syn{};
        fillHeader(syn, 0, FLG_SYN, static_cast<uint8_t>(attempt));
        sendPacket(send_fd_, send_dest, syn);

        std::cerr << "peel[" << config_.rank << "]: SYN attempt " << attempt
                  << ", syn=" << syn_count << "/" << config_.world_size
                  << ", ack=" << ack_count << "/" << config_.world_size << "\n";

        // Listen for RTO duration
        auto rto_end = Clock::now() + std::chrono::milliseconds(config_.rto_ms);

        while (Clock::now() < rto_end) {
            fd_set fds;
            FD_ZERO(&fds);
            int max_fd = -1;

            for (int r = 0; r < config_.world_size; ++r) {
                if (r == config_.rank || recv_fds_[r] < 0) continue;
                FD_SET(recv_fds_[r], &fds);
                if (recv_fds_[r] > max_fd) max_fd = recv_fds_[r];
            }
            if (max_fd < 0) break;

            timeval tv{0, 10000};  // 10ms
            int ready = select(max_fd + 1, &fds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            for (int r = 0; r < config_.world_size; ++r) {
                if (r == config_.rank) continue;
                int fd = recv_fds_[r];
                if (fd < 0 || !FD_ISSET(fd, &fds)) continue;

                sockaddr_in from{};
                socklen_t fromlen = sizeof(from);
                uint8_t buf[128];

                ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&from), &fromlen);
                if (n < static_cast<ssize_t>(sizeof(PeelHeader))) continue;

                PeelHeader hdr{};
                std::memcpy(&hdr, buf, sizeof(hdr));
                if (!peel_verify_header_checksum(hdr)) continue;

                uint16_t flags = ntohs(hdr.flags);
                int src_rank = hdr.rank;

                if (src_rank != r) continue;

                // Process SYN
                if ((flags & FLG_SYN) && !(flags & FLG_ACK) && !heard_syn[r]) {
                    heard_syn[r] = true;
                    ++syn_count;
                    result.peers[r] = from;

                    std::cerr << "peel[" << config_.rank << "]: SYN from rank " << r
                              << " (" << peel_addr_to_string(from) << ")\n";

                    // Reply with SYN+ACK
                    PeelHeader ack{};
                    fillHeader(ack, 0, FLG_SYN | FLG_ACK, hdr.retrans_id);
                    ack.tsecr = hdr.tsval;
                    peel_set_header_checksum(ack);
                    sendPacket(send_fd_, send_dest, ack);
                }

                // Process ACK
                if ((flags & FLG_ACK) && !heard_ack[r]) {
                    heard_ack[r] = true;
                    ++ack_count;
                    std::cerr << "peel[" << config_.rank << "]: ACK from rank " << r << "\n";
                }
            }
        }
    }

    if (syn_count < config_.world_size) {
        std::cerr << "peel[" << config_.rank << "]: timeout, syn=" << syn_count
                  << "/" << config_.world_size << "\n";
        return false;
    }

    // Send START
    PeelHeader startPkt{};
    fillHeader(startPkt, 0, FLG_START, 0);
    sendPacket(send_fd_, send_dest, startPkt);
    std::cerr << "peel[" << config_.rank << "]: sent START\n";

    return true;
}

int PeelFullMesh::createSocket(uint16_t port, bool is_sender) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(fd);
        return -1;
    }

    // Join multicast
    ip_mreq mreq{};
    mreq.imr_multiaddr = mcast_base_.sin_addr;
    if (!config_.iface_ip.empty()) {
        inet_pton(AF_INET, config_.iface_ip.c_str(), &mreq.imr_interface);
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ::close(fd);
        return -1;
    }

    if (is_sender) {
        if (!config_.iface_ip.empty()) {
            in_addr ifaddr{};
            inet_pton(AF_INET, config_.iface_ip.c_str(), &ifaddr);
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
        }
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &config_.ttl, sizeof(config_.ttl));
        int loop = 0;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    }

    if (config_.rcvbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &config_.rcvbuf, sizeof(config_.rcvbuf));
    }

    return fd;
}

void PeelFullMesh::fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags, uint8_t retrans_id) {
    peel_fill_header(h, seq, flags, config_.sendPort(),
                     static_cast<uint8_t>(config_.rank), retrans_id);
}

bool PeelFullMesh::sendPacket(int fd, const sockaddr_in& dest, const PeelHeader& hdr) {
    PeelHeader tmp = hdr;
    peel_set_header_checksum(tmp);
    ssize_t n = sendto(fd, &tmp, sizeof(tmp), 0,
                       reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    return n == sizeof(tmp);
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo