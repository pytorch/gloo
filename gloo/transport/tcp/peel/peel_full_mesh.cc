// gloo/transport/tcp/peel/peel_full_mesh.cc

#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

// =============================================================================
// Static helpers (mirror reference peel_sender/receiver)
// =============================================================================

static bool get_iface_index(const std::string& iface, int& idx) {
    idx = (int)if_nametoindex(iface.c_str());
    if (idx == 0) { perror(("if_nametoindex(" + iface + ")").c_str()); return false; }
    return true;
}

static bool get_iface_mac(const std::string& iface, uint8_t mac[6]) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket(get_iface_mac)"); return false; }
    ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
        perror(("SIOCGIFHWADDR " + iface).c_str()); ::close(s); return false;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    ::close(s);
    return true;
}

static bool get_iface_ip(const std::string& iface, uint32_t& ip_n) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket(get_iface_ip)"); return false; }
    ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
        perror(("SIOCGIFADDR " + iface).c_str()); ::close(s); return false;
    }
    ip_n = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr.s_addr;
    ::close(s);
    return true;
}

// Derive standard Ethernet multicast MAC from IPv4 multicast address (RFC 1112).
static void mcast_ip_to_mac(uint32_t mcast_ip_n, uint8_t mac[6]) {
    uint32_t ip = ntohl(mcast_ip_n);
    mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
    mac[3] = (ip >> 16) & 0x7f;  // bit 23 cleared per RFC
    mac[4] = (ip >>  8) & 0xff;
    mac[5] =  ip        & 0xff;
}

// Attach kernel BPF filter: only deliver frames matching
// (proto=UDP AND dst_ip==filter_dst_ip_n AND dst_port==filter_dst_port_h).
// Non-matching frames are dropped before reaching userspace.
static bool attach_rx_filter(int fd, uint32_t filter_dst_ip_n, uint16_t filter_dst_port_h) {
    sock_filter f[] = {
        /*[0]*/ BPF_STMT(BPF_LD |BPF_B|BPF_ABS,  23),
        /*[1]*/ BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,  IPPROTO_UDP,            0, 6),
        /*[2]*/ BPF_STMT(BPF_LD |BPF_W|BPF_ABS,  30),
        /*[3]*/ BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,  ntohl(filter_dst_ip_n), 0, 4),
        /*[4]*/ BPF_STMT(BPF_LDX|BPF_B|BPF_MSH,  14),
        /*[5]*/ BPF_STMT(BPF_LD |BPF_H|BPF_IND,  16),
        /*[6]*/ BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,  filter_dst_port_h,      0, 1),
        /*[7]*/ BPF_STMT(BPF_RET|BPF_K, 0xFFFFFFFF),
        /*[8]*/ BPF_STMT(BPF_RET|BPF_K, 0),
    };
    sock_fprog prog{ static_cast<unsigned short>(sizeof(f) / sizeof(f[0])), f };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0) {
        perror("SO_ATTACH_FILTER (non-fatal)");
        return false;
    }
    return true;
}

// Build a complete Ethernet frame: EthHdr(14)+Ip4Hdr(20)+UdpHdr(8)+payload.
// src/dst ports and IPs: ports in host order, IPs in network order.
// Returns total frame length, or 0 if cap is too small.
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
    uint8_t        tos,
    const uint8_t* payload,
    size_t         payload_len)
{
    const size_t udp_len   = 8 + payload_len;
    const size_t ip_len    = 20 + udp_len;
    const size_t frame_len = 14 + ip_len;
    if (frame_len > cap) return 0;

    // Ethernet header
    memcpy(frame,     dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;  // EtherType IPv4

    // IP header
    auto* ip    = reinterpret_cast<iphdr*>(frame + 14);
    ip->ihl      = 5;
    ip->version  = 4;
    ip->tos      = tos;
    ip->tot_len  = htons((uint16_t)ip_len);
    ip->id       = htons(ip_id);
    ip->frag_off = htons(0x4000);  // DF bit
    ip->ttl      = ttl;
    ip->protocol = IPPROTO_UDP;
    ip->check    = 0;
    ip->saddr    = src_ip_n;
    ip->daddr    = dst_ip_n;
    ip->check    = peel_checksum16(ip, 20);

    // UDP header
    auto* udp   = reinterpret_cast<udphdr*>(frame + 14 + 20);
    udp->source  = htons(src_port_h);
    udp->dest    = htons(dst_port_h);
    udp->len     = htons((uint16_t)udp_len);
    udp->check   = 0;  // optional for IPv4

    if (payload_len > 0)
        memcpy(frame + 14 + 20 + 8, payload, payload_len);

    return frame_len;
}

// Parse a raw Ethernet frame. Only accepts EtherType=IPv4, proto=UDP.
// filter_dst_ip_n=0 skips IP check; filter_dst_port_h=0 skips port check.
// Fills src_ip_n (network order) and src_port_h (host order).
// Returns pointer to UDP payload, or nullptr on mismatch/error.
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
    if (((frame[12] << 8) | frame[13]) != 0x0800) return nullptr;  // not IPv4

    const auto* ip = reinterpret_cast<const iphdr*>(frame + 14);
    if (ip->version != 4) return nullptr;
    int ihl = ip->ihl * 4;
    if (ihl < 20) return nullptr;
    if (ip->protocol != IPPROTO_UDP) return nullptr;
    if (filter_dst_ip_n && ip->daddr != filter_dst_ip_n) return nullptr;

    uint16_t ip_tot = ntohs(ip->tot_len);
    if ((ssize_t)(14 + ip_tot) > n)    return nullptr;
    if ((ssize_t)(14 + ihl + 8) > n)   return nullptr;

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
      mcast(o.mcast), ll_dest(o.ll_dest), is_sender(o.is_sender) {
    o.fd = -1;
}

PeelChannel& PeelChannel::operator=(PeelChannel&& o) noexcept {
    if (this != &o) {
        if (fd >= 0) ::close(fd);
        fd         = o.fd;
        owner_rank = o.owner_rank;
        port       = o.port;
        mcast      = o.mcast;
        ll_dest    = o.ll_dest;
        is_sender  = o.is_sender;
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
    memset(src_mac_, 0, sizeof(src_mac_));
}

PeelFullMesh::~PeelFullMesh() {
    if (send_fd_ >= 0) { ::close(send_fd_); send_fd_ = -1; }
    for (auto& fd : recv_fds_) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
}

bool PeelFullMesh::init() {
    std::cerr << "peel[" << config_.rank << "]: initializing...\n";

    // Derive interface properties from name — no strings in config for MAC/IP
    if (!get_iface_index(config_.iface_name, if_idx_)) {
        std::cerr << "peel[" << config_.rank << "]: iface index failed\n";
        return false;
    }
    if (!get_iface_mac(config_.iface_name, src_mac_)) {
        std::cerr << "peel[" << config_.rank << "]: iface mac failed\n";
        return false;
    }
    if (!get_iface_ip(config_.iface_name, src_ip_n_)) {
        std::cerr << "peel[" << config_.rank << "]: iface ip failed\n";
        return false;
    }

    if (!createSockets()) {
        std::cerr << "peel[" << config_.rank << "]: socket creation failed\n";
        return false;
    }

    std::cerr << "peel[" << config_.rank << "]: initialized\n";
    return true;
}

std::unique_ptr<PeelFullMeshResult> PeelFullMesh::run() {
    std::cerr << "peel[" << config_.rank << "]: starting handshake...\n";

    // No Redis barrier here — PeelDiscovery already acted as the barrier by
    // requiring all ranks to publish their IPs before any transport is created.

    auto result = std::make_unique<PeelFullMeshResult>();
    result->rank       = config_.rank;
    result->world_size = config_.world_size;
    result->peers.resize(config_.world_size);
    result->src_ip_n   = src_ip_n_;
    result->if_idx     = if_idx_;
    memcpy(result->src_mac, src_mac_, 6);

    if (!performHandshake(*result)) {
        std::cerr << "peel[" << config_.rank << "]: handshake failed\n";
        return nullptr;
    }

    // Build the L2 destination used for all outgoing frames.
    // Use the transport's fixed ruleset MAC if configured, otherwise
    // derive the standard multicast MAC from the group IP.
    uint8_t dst_mac[6];
    if (config_.use_cidr_rules_mac) {
        memcpy(dst_mac, config_.cidr_rules_mac, 6);
    } else {
        mcast_ip_to_mac(mcast_base_.sin_addr.s_addr, dst_mac);
    }

    sockaddr_ll ll{};
    ll.sll_family   = AF_PACKET;
    ll.sll_protocol = htons(ETH_P_IP);
    ll.sll_ifindex  = if_idx_;
    ll.sll_halen    = 6;
    memcpy(ll.sll_addr, dst_mac, 6);

    // Transfer send channel
    result->send_channel = std::make_unique<PeelChannel>();
    result->send_channel->fd         = send_fd_;
    result->send_channel->owner_rank = config_.rank;
    result->send_channel->port       = config_.sendPort();
    result->send_channel->mcast      = mcast_base_;
    result->send_channel->mcast.sin_port = htons(config_.sendPort());
    result->send_channel->ll_dest    = ll;
    result->send_channel->is_sender  = true;
    send_fd_ = -1;

    // Transfer receive channels
    for (int r = 0; r < config_.world_size; ++r) {
        if (r == config_.rank || recv_fds_[r] < 0) continue;

        auto ch = std::make_unique<PeelChannel>();
        ch->fd         = recv_fds_[r];
        ch->owner_rank = r;
        ch->port       = config_.recvPort(r);
        ch->mcast      = mcast_base_;
        ch->mcast.sin_port = htons(config_.recvPort(r));
        ch->ll_dest    = ll;
        ch->is_sender  = false;
        recv_fds_[r]   = -1;

        result->recv_channels.push_back(std::move(ch));
    }

    std::cerr << "peel[" << config_.rank << "]: handshake complete\n";
    return result;
}

// =============================================================================
// Internal
// =============================================================================

bool PeelFullMesh::createSockets() {
    std::memset(&mcast_base_, 0, sizeof(mcast_base_));
    mcast_base_.sin_family = AF_INET;
    if (inet_pton(AF_INET, config_.mcast_group.c_str(), &mcast_base_.sin_addr) != 1) {
        std::cerr << "peel: invalid mcast group: " << config_.mcast_group << "\n";
        return false;
    }

    send_fd_ = createSocket(config_.sendPort(), true);
    if (send_fd_ < 0) {
        std::cerr << "peel: send socket failed\n";
        return false;
    }

    // Only open receive sockets for the ranks that are actually in this mesh.
    // When participant_ranks is empty, all ranks 0..world_size-1 participate.
    const std::vector<int>* parts = &config_.participant_ranks;
    std::vector<int> all_ranks;
    if (parts->empty()) {
        all_ranks.reserve(config_.world_size);
        for (int r = 0; r < config_.world_size; ++r) all_ranks.push_back(r);
        parts = &all_ranks;
    }

    for (int r : *parts) {
        if (r == config_.rank) continue;
        recv_fds_[r] = createSocket(config_.recvPort(r), false);
        if (recv_fds_[r] < 0) {
            std::cerr << "peel: recv socket failed for rank " << r << "\n";
            return false;
        }
    }

    return true;
}

bool PeelFullMesh::performHandshake(PeelFullMeshResult& result) {
    auto start   = Clock::now();
    auto timeout = std::chrono::milliseconds(config_.handshake_timeout_ms);

    sockaddr_in send_dest = mcast_base_;
    send_dest.sin_port = htons(config_.sendPort());

    // =========================================================================
    // SENDER PATH  (this rank is the data sender for this mesh)
    //
    // 1. Broadcast SYN (multicast) until unicast ACKs arrive from all receivers.
    //    Receivers learn the sender is alive from the SYN and reply with a
    //    unicast ACK addressed to our own IP + sendPort (BPF-filtered on send_fd_).
    // 2. Broadcast START to unblock all receivers simultaneously.
    // =========================================================================
    if (config_.rank == config_.sender_rank) {

        // Resolve participant list — needed to know expected ACK count.
        const std::vector<int>* parts = &config_.participant_ranks;
        std::vector<int> all_ranks;
        if (parts->empty()) {
            all_ranks.reserve(config_.world_size);
            for (int r = 0; r < config_.world_size; ++r) all_ranks.push_back(r);
            parts = &all_ranks;
        }
        const int participant_count = static_cast<int>(parts->size());

        // Track which receivers have ACKed. Indexed by rank for O(1) lookup.
        std::vector<bool> heard_ack(config_.world_size, false);
        heard_ack[config_.rank] = true;  // don't wait for ACK from self
        int ack_count = 1;

        constexpr int kMaxAttempts = 20;

        for (int attempt = 1;
             ack_count < participant_count &&
             Clock::now() - start < timeout && attempt <= kMaxAttempts;
             ++attempt)
        {
            PeelHeader syn{};
            fillHeader(syn, 0, FLG_SYN, static_cast<uint8_t>(attempt));
            sendPacket(send_fd_, send_dest, syn);

            std::cerr << "peel[" << config_.rank << "]: SYN attempt " << attempt
                      << ", ack=" << ack_count << "/" << participant_count << "\n";

            // Collect unicast ACKs on send_fd_ within one RTO window.
            // BPF filter on send_fd_ already restricts to: dst_ip=own, dst_port=sendPort().
            auto rto_end = Clock::now() + std::chrono::milliseconds(config_.rto_ms);
            while (Clock::now() < rto_end && ack_count < participant_count) {
                uint8_t buf[2048];
                ssize_t n = ::recv(send_fd_, buf, sizeof(buf), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    break;
                }

                uint32_t src_ip; uint16_t src_port; size_t plen;
                const uint8_t* payload = parse_udp_frame(
                    buf, n,
                    src_ip_n_,           // ACKs must be addressed to our unicast IP
                    config_.sendPort(),  // and to our send port
                    src_ip, src_port, plen);
                if (!payload || plen < sizeof(PeelHeader)) continue;

                PeelHeader hdr{};
                std::memcpy(&hdr, payload, sizeof(hdr));
                if (!peel_verify_header_checksum(hdr)) continue;
                if (!(ntohs(hdr.flags) & FLG_ACK)) continue;

                const int src_rank = static_cast<int>(hdr.rank);
                if (src_rank < 0 || src_rank >= config_.world_size) continue;
                if (heard_ack[src_rank]) continue;

                heard_ack[src_rank] = true;
                ++ack_count;
                result.peers[src_rank].sin_family      = AF_INET;
                result.peers[src_rank].sin_addr.s_addr = src_ip;
                result.peers[src_rank].sin_port        = htons(src_port);
                std::cerr << "peel[" << config_.rank << "]: ACK from rank " << src_rank << "\n";
            }
        }

        if (ack_count < participant_count) {
            std::cerr << "peel[" << config_.rank << "]: handshake timeout, ack="
                      << ack_count << "/" << participant_count << "\n";
            return false;
        }

        // All receivers ready — broadcast START to signal data phase.
        PeelHeader startPkt{};
        fillHeader(startPkt, 0, FLG_START, 0);
        sendPacket(send_fd_, send_dest, startPkt);
        std::cerr << "peel[" << config_.rank << "]: sent START\n";
        return true;
    }

    // =========================================================================
    // RECEIVER PATH  (this rank only receives data, never multicasts)
    //
    // 1. Wait on recv_fds_[sender_rank] for a SYN from the sender.
    //    Learn the sender's unicast IP and MAC from the received frame.
    // 2. Unicast-ACK back to the sender (no multicast needed).
    //    Re-ACK every SYN retransmit in case a previous ACK was lost.
    // 3. Wait for START before returning — guarantees all receivers are
    //    unblocked at the same moment the sender begins transmitting.
    // =========================================================================
    const int sfd = recv_fds_[config_.sender_rank];
    if (sfd < 0) {
        std::cerr << "peel[" << config_.rank
                  << "]: no recv socket for sender rank " << config_.sender_rank << "\n";
        return false;
    }

    // Set a receive timeout so we don't block forever if sender never arrives.
    timeval tv{};
    tv.tv_sec  = config_.handshake_timeout_ms / 1000;
    tv.tv_usec = (config_.handshake_timeout_ms % 1000) * 1000;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t sender_ip_n = 0;
    uint8_t  sender_mac[6]{};
    bool got_syn   = false;
    bool got_start = false;

    while (!got_start && Clock::now() - start < timeout) {
        uint8_t buf[2048];
        ssize_t n = ::recv(sfd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }

        uint32_t src_ip; uint16_t src_port; size_t plen;
        const uint8_t* payload = parse_udp_frame(
            buf, n,
            mcast_base_.sin_addr.s_addr,          // sender multicasts to group
            config_.recvPort(config_.sender_rank), // on sender's own port
            src_ip, src_port, plen);
        if (!payload || plen < sizeof(PeelHeader)) continue;

        PeelHeader hdr{};
        std::memcpy(&hdr, payload, sizeof(hdr));
        if (!peel_verify_header_checksum(hdr)) continue;
        if (static_cast<int>(hdr.rank) != config_.sender_rank) continue;

        const uint16_t flags = ntohs(hdr.flags);

        if (flags & FLG_SYN) {
            // Capture sender info on first SYN (unchanged on retransmits).
            if (!got_syn) {
                got_syn      = true;
                sender_ip_n  = src_ip;
                if (n >= 12) memcpy(sender_mac, buf + 6, 6);  // Ethernet src MAC
                result.peers[config_.sender_rank].sin_family      = AF_INET;
                result.peers[config_.sender_rank].sin_addr.s_addr = src_ip;
                result.peers[config_.sender_rank].sin_port        = htons(src_port);
                std::cerr << "peel[" << config_.rank << "]: SYN from sender rank "
                          << config_.sender_rank << "\n";
            }

            // Always unicast-ACK the SYN (including retransmits) so the sender
            // can count us even if a previous ACK was dropped in transit.
            sockaddr_in ack_dest{};
            ack_dest.sin_family      = AF_INET;
            ack_dest.sin_addr.s_addr = sender_ip_n;
            // Sender's BPF filter on send_fd_: dst_ip=sender_ip, dst_port=sendPort().
            ack_dest.sin_port = htons(config_.recvPort(config_.sender_rank));

            PeelHeader ack{};
            fillHeader(ack, 0, FLG_ACK, hdr.retrans_id);
            // Use sender's unicast MAC so the ACK routes directly to the sender.
            sendPacket(send_fd_, ack_dest, ack, sender_mac);
            std::cerr << "peel[" << config_.rank << "]: unicast ACK to sender\n";
        }

        if (flags & FLG_START) {
            got_start = true;
            std::cerr << "peel[" << config_.rank << "]: received START\n";
        }
    }

    if (!got_start) {
        std::cerr << "peel[" << config_.rank
                  << "]: handshake timeout waiting for START\n";
        return false;
    }
    return true;
}

int PeelFullMesh::createSocket(uint16_t port, bool is_sender) {
    // AF_PACKET + SOCK_RAW: kernel adds nothing, we build full Ethernet frames
    int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (fd < 0) { perror("socket(AF_PACKET)"); return -1; }

    // Bind to the interface by index
    sockaddr_ll local{};
    local.sll_family   = AF_PACKET;
    local.sll_protocol = htons(ETH_P_IP);
    local.sll_ifindex  = if_idx_;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        perror("bind(AF_PACKET)"); ::close(fd); return -1;
    }

    // Join multicast at L2 — register the standard RFC 1112 multicast MAC with
    // the NIC filter so the hardware delivers multicast frames to the socket.
    // The CIDR subtree MAC is only used by the sender as the Ethernet destination
    // into the switch fabric; switches rewrite it to a standard multicast MAC at
    // the last hop, so receivers always see the standard MAC and only need this
    // one registration.
    uint8_t mcast_mac[6];
    mcast_ip_to_mac(mcast_base_.sin_addr.s_addr, mcast_mac);
    packet_mreq mr{};
    mr.mr_ifindex = if_idx_;
    mr.mr_type    = PACKET_MR_MULTICAST;
    mr.mr_alen    = 6;
    memcpy(mr.mr_address, mcast_mac, 6);
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
        perror("PACKET_ADD_MEMBERSHIP (non-fatal)");

    if (config_.rcvbuf > 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &config_.rcvbuf, sizeof(config_.rcvbuf));

    if (is_sender) {
        // Sender socket receives unicast ACKs addressed to our own IP:sendPort
        attach_rx_filter(fd, src_ip_n_, config_.sendPort());

        // SO_RCVTIMEO drives the ACK-collection deadline in waitForAcks
        timeval tv{};
        tv.tv_sec  = config_.rto_ms / 1000;
        tv.tv_usec = (config_.rto_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    } else {
        // Recv socket receives multicast frames addressed to our group on rank r's port
        attach_rx_filter(fd, mcast_base_.sin_addr.s_addr, port);
    }

    return fd;
}

void PeelFullMesh::fillHeader(PeelHeader& h, uint32_t seq, uint16_t flags, uint8_t retrans_id) {
    peel_fill_header(h, seq, flags, config_.sendPort(),
                     static_cast<uint8_t>(config_.rank), retrans_id);
}

bool PeelFullMesh::sendPacket(int fd, const sockaddr_in& dest, const PeelHeader& hdr,
                               const uint8_t* dst_mac_override) {
    PeelHeader tmp = hdr;
    peel_set_header_checksum(tmp);

    // Destination MAC priority:
    //   1. dst_mac_override — used for unicast frames (e.g. receiver ACK to sender)
    //   2. cidr_rules_mac   — used when CIDR subtree routing is active
    //   3. standard RFC 1112 multicast MAC derived from mcast group IP
    uint8_t dst_mac[6];
    if (dst_mac_override) {
        memcpy(dst_mac, dst_mac_override, 6);
    } else if (config_.use_cidr_rules_mac) {
        memcpy(dst_mac, config_.cidr_rules_mac, 6);
    } else {
        mcast_ip_to_mac(mcast_base_.sin_addr.s_addr, dst_mac);
    }

    uint8_t frame[14 + 20 + 8 + sizeof(PeelHeader)];
    size_t flen = build_udp_frame(
        frame, sizeof(frame),
        src_mac_, dst_mac,
        src_ip_n_, dest.sin_addr.s_addr,
        config_.sendPort(), ntohs(dest.sin_port),
        (uint8_t)config_.ttl,
        ip_id_++,
        static_cast<uint8_t>(config_.dscp << 2),
        reinterpret_cast<const uint8_t*>(&tmp), sizeof(tmp));

    if (flen == 0) return false;

    sockaddr_ll sll{};
    sll.sll_family  = AF_PACKET;
    sll.sll_ifindex = if_idx_;
    sll.sll_halen   = 6;
    memcpy(sll.sll_addr, dst_mac, 6);

    ssize_t n = sendto(fd, frame, flen, 0,
                       reinterpret_cast<const sockaddr*>(&sll), sizeof(sll));
    return n == (ssize_t)flen;
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
