// gloo/transport/tcp/peel/peel_transport.cc

#include "peel_transport.h"
#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_set>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

// =============================================================================
// Static frame helpers
// =============================================================================

static size_t build_udp_frame(
    uint8_t*       frame,
    size_t         cap,
    const uint8_t  src_mac[6],
    const uint8_t  dst_mac[6],
    uint32_t       src_ip_n,
    uint32_t       dst_ip_n,
    uint16_t       src_port_h,
    uint16_t       dst_port_h,
    uint8_t        ttl,
    uint16_t       ip_id,
    const uint8_t* payload,
    size_t         payload_len)
{
    const size_t udp_len   = 8 + payload_len;
    const size_t ip_len    = 20 + udp_len;
    const size_t frame_len = 14 + ip_len;
    if (frame_len > cap) return 0;

    memcpy(frame,     dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    auto* ip    = reinterpret_cast<iphdr*>(frame + 14);
    ip->ihl      = 5;
    ip->version  = 4;
    ip->tos      = 0;
    ip->tot_len  = htons((uint16_t)ip_len);
    ip->id       = htons(ip_id);
    ip->frag_off = htons(0x4000);  // DF
    ip->ttl      = ttl;
    ip->protocol = IPPROTO_UDP;
    ip->check    = 0;
    ip->saddr    = src_ip_n;
    ip->daddr    = dst_ip_n;
    ip->check    = peel_checksum16(ip, 20);

    auto* udp   = reinterpret_cast<udphdr*>(frame + 14 + 20);
    udp->source  = htons(src_port_h);
    udp->dest    = htons(dst_port_h);
    udp->len     = htons((uint16_t)udp_len);
    udp->check   = 0;

    if (payload_len > 0)
        memcpy(frame + 14 + 20 + 8, payload, payload_len);

    return frame_len;
}

static const uint8_t* parse_udp_frame(
    const uint8_t* frame,
    ssize_t        n,
    uint32_t       filter_dst_ip_n,
    uint16_t       filter_dst_port_h,
    uint32_t&      src_ip_n,
    uint16_t&      src_port_h,
    size_t&        payload_len)
{
    if (n < (ssize_t)(14 + 20 + 8)) return nullptr;
    if (((frame[12] << 8) | frame[13]) != 0x0800) return nullptr;

    const auto* ip = reinterpret_cast<const iphdr*>(frame + 14);
    if (ip->version != 4) return nullptr;
    int ihl = ip->ihl * 4;
    if (ihl < 20) return nullptr;
    if (ip->protocol != IPPROTO_UDP) return nullptr;
    if (filter_dst_ip_n && ip->daddr != filter_dst_ip_n) return nullptr;

    uint16_t ip_tot = ntohs(ip->tot_len);
    if ((ssize_t)(14 + ip_tot) > n)  return nullptr;
    if ((ssize_t)(14 + ihl + 8) > n) return nullptr;

    const auto* udp = reinterpret_cast<const udphdr*>(frame + 14 + ihl);
    if (filter_dst_port_h && ntohs(udp->dest) != filter_dst_port_h) return nullptr;

    uint16_t udp_len_val = ntohs(udp->len);
    if (udp_len_val < 8) return nullptr;

    src_ip_n    = ip->saddr;
    src_port_h  = ntohs(udp->source);
    payload_len = udp_len_val - 8;
    return reinterpret_cast<const uint8_t*>(udp) + 8;
}

// =============================================================================
// PeelTransport
// =============================================================================

PeelTransport::PeelTransport(const PeelTransportConfig& config)
    : config_(config) {}

PeelTransport::~PeelTransport() = default;

bool PeelTransport::init() {
    PeelFullMeshConfig mesh_config;
    mesh_config.mcast_group          = config_.mcast_group;
    mesh_config.base_port            = config_.base_port;
    mesh_config.rank                 = config_.rank;
    mesh_config.world_size           = config_.world_size;
    mesh_config.iface_name           = config_.iface_name;
    mesh_config.ttl                  = config_.ttl;
    mesh_config.rcvbuf               = config_.rcvbuf;
    mesh_config.rto_ms               = config_.rto_ms;
    mesh_config.handshake_timeout_ms = config_.timeout_ms;
    mesh_config.redis_host           = config_.redis_host;
    mesh_config.redis_port           = config_.redis_port;
    mesh_config.redis_prefix         = config_.redis_prefix;

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
    if (!isReady() || !mesh_result_->send_channel) return false;

    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    uint32_t seq = next_seq_;

    while (remaining > 0) {
        size_t chunk = std::min(remaining, config_.max_chunk_size);
        uint16_t flags = FLG_DATA;
        if (remaining == size)  flags |= FLG_SYN;
        if (remaining <= chunk) flags |= FLG_FIN;

        // Stop-and-wait: retry up to PEEL_DEFAULT_RETRIES times per packet
        bool acked = false;
        for (int attempt = 0; attempt < PEEL_DEFAULT_RETRIES && !acked; ++attempt) {
            if (!sendPacket(seq, flags, ptr, chunk)) return false;

            // Single-rank case: no receivers, nothing to wait for
            if (config_.world_size == 1) { acked = true; break; }

            acked = waitForAcks(seq, config_.rto_ms);
            if (!acked)
                std::cerr << "peel_transport[" << config_.rank << "]: timeout seq="
                          << seq << " attempt=" << attempt + 1 << ", retransmitting\n";
        }

        if (!acked) {
            std::cerr << "peel_transport[" << config_.rank
                      << "]: failed to deliver seq=" << seq << "\n";
            return false;
        }

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

    // Build PeelHeader + app payload as the UDP payload
    PeelHeader hdr{};
    peel_fill_header(hdr, seq, flags, ch->port,
                     static_cast<uint8_t>(config_.rank), 1);
    peel_set_header_checksum(hdr);

    std::vector<uint8_t> udp_payload(PEEL_HEADER_SIZE + len);
    std::memcpy(udp_payload.data(), &hdr, PEEL_HEADER_SIZE);
    if (len > 0)
        std::memcpy(udp_payload.data() + PEEL_HEADER_SIZE, payload, len);

    // Derive multicast dst MAC from group IP
    uint32_t mcast_ip = ch->mcast.sin_addr.s_addr;
    uint8_t  dst_mac[6];
    {
        uint32_t ip = ntohl(mcast_ip);
        dst_mac[0] = 0x01; dst_mac[1] = 0x00; dst_mac[2] = 0x5e;
        dst_mac[3] = (ip >> 16) & 0x7f;
        dst_mac[4] = (ip >>  8) & 0xff;
        dst_mac[5] =  ip        & 0xff;
    }

    std::vector<uint8_t> frame(14 + 20 + 8 + udp_payload.size());
    size_t flen = build_udp_frame(
        frame.data(), frame.size(),
        mesh_result_->src_mac, dst_mac,
        mesh_result_->src_ip_n, mcast_ip,
        ch->port, ntohs(ch->mcast.sin_port),
        (uint8_t)config_.ttl,
        ip_id_++,
        udp_payload.data(), udp_payload.size());

    if (flen == 0) return false;

    sockaddr_ll sll = ch->ll_dest;
    ssize_t n = sendto(ch->fd, frame.data(), flen, 0,
                       reinterpret_cast<const sockaddr*>(&sll), sizeof(sll));
    return n == (ssize_t)flen;
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
    bool   done     = false;

    auto deadline = Clock::now() + std::chrono::milliseconds(
        timeout_ms > 0 ? timeout_ms : 30000);

    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 100000;  // 100ms poll
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!done && Clock::now() < deadline && received < max_size) {
        uint8_t buf[65536];
        ssize_t n = ::recv(ch->fd, buf, sizeof(buf), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return -1;
        }

        // Extract sender MAC from Ethernet header (offset 6) for ACK addressing
        uint8_t sender_mac[6]{};
        if (n >= 12) memcpy(sender_mac, buf + 6, 6);

        uint32_t src_ip; uint16_t src_port; size_t plen;
        const uint8_t* payload = parse_udp_frame(
            buf, n,
            ch->mcast.sin_addr.s_addr,  // dst must be our multicast group
            ch->port,                    // dst port must match this channel
            src_ip, src_port, plen);

        if (!payload || plen < PEEL_HEADER_SIZE) continue;

        PeelHeader hdr{};
        std::memcpy(&hdr, payload, sizeof(hdr));
        if (!peel_verify_header_checksum(hdr)) continue;
        if (hdr.rank != from_rank) continue;

        uint16_t flags = ntohs(hdr.flags);
        if (!(flags & FLG_DATA)) continue;

        size_t app_len = plen - PEEL_HEADER_SIZE;
        if (received + app_len > max_size)
            app_len = max_size - received;

        if (app_len > 0) {
            std::memcpy(out + received, payload + PEEL_HEADER_SIZE, app_len);
            received += app_len;
        }

        // Send unicast ACK back to sender using learned MAC
        sendAck(src_ip, ntohs(hdr.src_port), sender_mac,
                ntohl(hdr.seq), ntohl(hdr.tsval), hdr.retrans_id);

        if (flags & FLG_FIN) done = true;
    }

    return done ? static_cast<ssize_t>(received) : -1;
}

bool PeelTransport::recvPacket(int from_rank, PeelHeader& hdr,
                               std::vector<uint8_t>& payload, int timeout_ms) {
    auto* ch = mesh_result_->getRecvChannel(from_rank);
    if (!ch || ch->fd < 0) return false;

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[65536];
    ssize_t n = ::recv(ch->fd, buf, sizeof(buf), 0);
    if (n < 0) return false;

    uint32_t src_ip; uint16_t src_port; size_t plen;
    const uint8_t* raw = parse_udp_frame(
        buf, n,
        ch->mcast.sin_addr.s_addr, ch->port,
        src_ip, src_port, plen);

    if (!raw || plen < PEEL_HEADER_SIZE) return false;

    std::memcpy(&hdr, raw, sizeof(hdr));
    if (!peel_verify_header_checksum(hdr)) return false;

    size_t data_len = plen - PEEL_HEADER_SIZE;
    if (data_len > 0)
        payload.assign(raw + PEEL_HEADER_SIZE, raw + PEEL_HEADER_SIZE + data_len);
    else
        payload.clear();

    return true;
}

// =============================================================================
// ACK helpers
// =============================================================================

void PeelTransport::sendAck(uint32_t dst_ip_n, uint16_t dst_port_h,
                            const uint8_t dst_mac[6],
                            uint32_t seq, uint32_t tsecr, uint8_t retrans_id) {
    auto* ch = mesh_result_->send_channel.get();
    if (!ch || ch->fd < 0) return;

    PeelHeader ack{};
    peel_fill_header(ack, seq, FLG_ACK, ch->port,
                     static_cast<uint8_t>(config_.rank), retrans_id,
                     peel_now_ms(), tsecr);
    peel_set_header_checksum(ack);

    uint8_t frame[14 + 20 + 8 + PEEL_HEADER_SIZE];
    size_t flen = build_udp_frame(
        frame, sizeof(frame),
        mesh_result_->src_mac, dst_mac,
        mesh_result_->src_ip_n, dst_ip_n,
        ch->port, dst_port_h,
        64,  // TTL for unicast ACK
        ip_id_++,
        reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));

    if (flen == 0) return;

    sockaddr_ll sll{};
    sll.sll_family  = AF_PACKET;
    sll.sll_ifindex = mesh_result_->if_idx;
    sll.sll_halen   = 6;
    memcpy(sll.sll_addr, dst_mac, 6);

    sendto(ch->fd, frame, flen, 0,
           reinterpret_cast<const sockaddr*>(&sll), sizeof(sll));
}

bool PeelTransport::waitForAcks(uint32_t seq, int timeout_ms) {
    if (!isReady()) return false;

    auto* ch = mesh_result_->send_channel.get();
    if (!ch || ch->fd < 0) return false;

    int expected = config_.world_size - 1;
    if (expected <= 0) return true;

    // Pack (src_ip, src_port) into a 64-bit key to track unique receivers
    std::unordered_set<uint64_t> got;
    got.reserve((size_t)expected * 2);

    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    while (Clock::now() < deadline) {
        uint8_t buf[2048];
        ssize_t n = ::recv(ch->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            // SO_RCVTIMEO expired or interrupted
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }

        uint32_t src_ip; uint16_t src_port; size_t plen;
        const uint8_t* payload = parse_udp_frame(
            buf, n,
            mesh_result_->src_ip_n,  // ACKs must be addressed to our unicast IP
            ch->port,                // and to our send port
            src_ip, src_port, plen);

        if (!payload || plen < PEEL_HEADER_SIZE) continue;

        PeelHeader hdr{};
        std::memcpy(&hdr, payload, sizeof(hdr));
        if (!peel_verify_header_checksum(hdr)) continue;
        if ((ntohs(hdr.flags) & FLG_ACK) == 0) continue;
        if (ntohl(hdr.seq) != seq) continue;

        // Unique key per receiver: (src_ip, src_port)
        uint64_t key = ((uint64_t)(uint32_t)src_ip << 16) | src_port;
        got.insert(key);
        if ((int)got.size() >= expected) return true;
    }

    return false;
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
