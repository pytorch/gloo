// gloo/transport/tcp/peel/peel_transport.cc

#include "peel_transport.h"
#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <sys/socket.h>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

PeelTransport::PeelTransport(const PeelTransportConfig& config)
    : config_(config) {}

PeelTransport::~PeelTransport() = default;

bool PeelTransport::init() {
    PeelFullMeshConfig mesh_config;
    mesh_config.mcast_group = config_.mcast_group;
    mesh_config.base_port = config_.base_port;
    mesh_config.rank = config_.rank;
    mesh_config.world_size = config_.world_size;
    mesh_config.iface_ip = config_.iface_ip;
    mesh_config.ttl = config_.ttl;
    mesh_config.rcvbuf = config_.rcvbuf;
    mesh_config.rto_ms = config_.rto_ms;
    mesh_config.handshake_timeout_ms = config_.timeout_ms;
    mesh_config.redis_host = config_.redis_host;
    mesh_config.redis_port = config_.redis_port;
    mesh_config.redis_prefix = config_.redis_prefix;

    PeelFullMesh mesh(mesh_config);

    if (!mesh.init()) {
        std::cerr << "peel_transport: mesh init failed\n";
        return false;
    }

    mesh_result_ = mesh.run();
    if (!mesh_result_) {
        std::cerr << "peel_transport: mesh handshake failed\n";
        return false;
    }

    std::cerr << "peel_transport[" << config_.rank << "]: ready\n";
    return true;
}

void PeelTransport::cleanup() {
    mesh_result_.reset();
}

// =============================================================================
// Broadcast
// =============================================================================

bool PeelTransport::broadcast(int root, void* data, size_t size) {
    if (!isReady()) {
        std::cerr << "peel_transport: not ready\n";
        return false;
    }

    if (config_.rank == root) {
        return send(data, size);
    } else {
        ssize_t n = recv(root, data, size, config_.timeout_ms);
        return n == static_cast<ssize_t>(size);
    }
}

// =============================================================================
// Send
// =============================================================================

bool PeelTransport::send(const void* data, size_t size) {
    if (!isReady() || !mesh_result_->send_channel) {
        return false;
    }

    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    uint32_t seq = next_seq_;

    while (remaining > 0) {
        size_t chunk = std::min(remaining, config_.max_chunk_size);
        uint16_t flags = FLG_DATA;
        if (remaining == size) flags |= FLG_SYN;
        if (remaining <= chunk) flags |= FLG_FIN;

        if (!sendPacket(seq, flags, ptr, chunk)) {
            return false;
        }

        // TODO: Reliability - wait for ACKs, retransmit if needed

        ptr += chunk;
        remaining -= chunk;
        ++seq;
    }

    next_seq_ = seq;
    return true;
}

bool PeelTransport::sendPacket(uint32_t seq, uint16_t flags,
                               const void* payload, size_t len) {
    auto* ch = mesh_result_->send_channel.get();
    if (!ch || ch->fd < 0) return false;

    std::vector<uint8_t> pkt(PEEL_HEADER_SIZE + len);

    PeelHeader hdr{};
    peel_fill_header(hdr, seq, flags, ch->port,
                     static_cast<uint8_t>(config_.rank), 1);
    peel_set_header_checksum(hdr);
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));

    if (len > 0) {
        std::memcpy(pkt.data() + PEEL_HEADER_SIZE, payload, len);
    }

    ssize_t n = sendto(ch->fd, pkt.data(), pkt.size(), 0,
                       reinterpret_cast<const sockaddr*>(&ch->mcast),
                       sizeof(ch->mcast));

    return n == static_cast<ssize_t>(pkt.size());
}

// =============================================================================
// Receive
// =============================================================================

ssize_t PeelTransport::recv(int from_rank, void* data, size_t max_size, int timeout_ms) {
    if (!isReady()) return -1;

    auto* ch = mesh_result_->getRecvChannel(from_rank);
    if (!ch || ch->fd < 0) return -1;

    auto* out = static_cast<uint8_t*>(data);
    size_t received = 0;
    bool done = false;

    auto deadline = Clock::now() + std::chrono::milliseconds(
        timeout_ms > 0 ? timeout_ms : 30000);

    // Set socket timeout
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms poll
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!done && Clock::now() < deadline && received < max_size) {
        uint8_t buf[65536];
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);

        ssize_t n = recvfrom(ch->fd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromlen);

        if (n < static_cast<ssize_t>(PEEL_HEADER_SIZE)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) continue;
            return -1;
        }

        PeelHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));

        if (!peel_verify_header_checksum(hdr)) continue;
        if (hdr.rank != from_rank) continue;

        uint16_t flags = ntohs(hdr.flags);
        if (!(flags & FLG_DATA)) continue;

        size_t payload_len = static_cast<size_t>(n) - PEEL_HEADER_SIZE;
        if (received + payload_len > max_size) {
            payload_len = max_size - received;
        }

        if (payload_len > 0) {
            std::memcpy(out + received, buf + PEEL_HEADER_SIZE, payload_len);
            received += payload_len;
        }

        // TODO: Send ACK back to sender

        if (flags & FLG_FIN) {
            done = true;
        }
    }

    return done ? static_cast<ssize_t>(received) : -1;
}

bool PeelTransport::recvPacket(int from_rank, PeelHeader& hdr,
                               std::vector<uint8_t>& payload, int timeout_ms) {
    auto* ch = mesh_result_->getRecvChannel(from_rank);
    if (!ch || ch->fd < 0) return false;

    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[65536];
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);

    ssize_t n = recvfrom(ch->fd, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);

    if (n < static_cast<ssize_t>(PEEL_HEADER_SIZE)) return false;

    std::memcpy(&hdr, buf, sizeof(hdr));
    if (!peel_verify_header_checksum(hdr)) return false;

    if (static_cast<size_t>(n) > PEEL_HEADER_SIZE) {
        payload.assign(buf + PEEL_HEADER_SIZE, buf + n);
    } else {
        payload.clear();
    }

    return true;
}

bool PeelTransport::waitForAcks(uint32_t seq, int timeout_ms) {
    // TODO: Implement reliability
    // For now, just return true (unreliable)
    (void)seq;
    (void)timeout_ms;
    return true;
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo