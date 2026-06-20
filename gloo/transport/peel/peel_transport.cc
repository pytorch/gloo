// gloo/transport/peel/peel_transport.cc

#include "peel_transport.h"
#include "peel_full_mesh.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_set>

namespace gloo {
namespace transport {
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
    uint8_t        tos,
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
    ip->tos      = tos;
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

static int get_interface_mtu(const std::string& iface_name) {
    if (iface_name.empty()) {
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface_name.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    int mtu = -1;
    if (::ioctl(fd, SIOCGIFMTU, &ifr) == 0) {
        mtu = ifr.ifr_mtu;
    }

    ::close(fd);
    return mtu;
}

static size_t max_payload_for_mtu(int mtu) {
    constexpr size_t kIpUdpPeelOverhead = 20 + 8 + PEEL_HEADER_SIZE;
    if (mtu <= 0 || static_cast<size_t>(mtu) <= kIpUdpPeelOverhead) {
        return 0;
    }
    return static_cast<size_t>(mtu) - kIpUdpPeelOverhead;
}

// =============================================================================
// PeelTransport
// =============================================================================

PeelTransport::PeelTransport(const PeelTransportConfig& config)
    : config_(config) {}

PeelTransport::~PeelTransport() {
    cleanup();
}

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
    mesh_config.participant_ranks    = config_.participant_ranks;
    mesh_config.use_cidr_rules_mac   = config_.use_cidr_rules_mac;
    if (config_.use_cidr_rules_mac)
        memcpy(mesh_config.cidr_rules_mac, config_.cidr_rules_mac, 6);
    mesh_config.dscp                 = config_.dscp;
    mesh_config.sender_rank          = config_.sender_rank;

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

    const int iface_mtu = get_interface_mtu(config_.iface_name);
    const size_t mtu_payload_cap = max_payload_for_mtu(iface_mtu);
    const size_t requested_max_chunk_size = config_.max_chunk_size;

    if (config_.max_chunk_size == 0) {
        config_.max_chunk_size = mtu_payload_cap > 0 ? mtu_payload_cap : PEEL_MAX_PAYLOAD;
    }

    if (requested_max_chunk_size != config_.max_chunk_size) {
        std::cerr << "peel_transport[" << config_.rank
                  << "]: max_chunk_size=" << config_.max_chunk_size
                  << " requested=" << requested_max_chunk_size
                  << " iface=" << config_.iface_name
                  << " mtu=" << iface_mtu << "\n";
    }

    // Start the worker thread now that sockets are ready.
    // The thread must be started after mesh_result_ is set so it can
    // safely access send/recv channels from the moment it wakes up.
    worker_ = std::thread(&PeelTransport::workerLoop, this);

    std::cerr << "peel_transport[" << config_.rank << "]: ready\n";
    return true;
}

void PeelTransport::cleanup() {
    // Signal the worker to exit, then join before releasing resources.
    // This order is critical: mesh_result_ (and its sockets) must not be
    // freed while the worker thread may still be inside send() or recv().
    {
        std::lock_guard<std::mutex> lock(mu_);
        worker_state_ = WorkerState::SHUTDOWN;
        cv_work_.notify_one();
    }
    if (worker_.joinable())
        worker_.join();

    // Safe to release resources now that the worker has fully exited.
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
    submitWork(root, data, size);
    return waitResult();
}

bool PeelTransport::executeBroadcast(int root, void* data, size_t size) {
    if (config_.rank == root) {
        return send(data, size);
    } else {
        ssize_t n = recv(root, data, size, config_.timeout_ms);
        return n == static_cast<ssize_t>(size);
    }
}

void PeelTransport::submitWork(int root, void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mu_);
    assert(worker_state_ == WorkerState::IDLE &&
           "submitWork() called while worker is not IDLE");
    work_item_    = {root, data, size};
    worker_state_ = WorkerState::WORKING;
    cv_work_.notify_one();
}

bool PeelTransport::waitResult() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_done_.wait(lock, [this] {
        return worker_state_ == WorkerState::DONE;
    });
    bool result   = work_result_;
    worker_state_ = WorkerState::IDLE;
    return result;
}

void PeelTransport::workerLoop() {
    while (true) {
        // Wait until the main thread submits work or requests shutdown.
        std::unique_lock<std::mutex> lock(mu_);
        cv_work_.wait(lock, [this] {
            return worker_state_ == WorkerState::WORKING ||
                   worker_state_ == WorkerState::SHUTDOWN;
        });

        if (worker_state_ == WorkerState::SHUTDOWN)
            break;

        // Copy the work item while holding the lock, then release before
        // executing so the main thread is not blocked during the operation.
        WorkItem item = work_item_;
        lock.unlock();

        bool result = executeBroadcast(item.root, item.data, item.size);

        // Publish the result and wake the waiting main thread.
        lock.lock();
        work_result_  = result;
        worker_state_ = WorkerState::DONE;
        cv_done_.notify_one();
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

        const int mesh_size = config_.participant_ranks.empty()
            ? config_.world_size
            : static_cast<int>(config_.participant_ranks.size());

        // Persistent ACK-tracking set: shared across all retransmit attempts for
        // this sequence number.  A receiver whose ACK was delayed past the first
        // rto_ms window has already inserted its frame into the socket buffer; if
        // we retransmit and open a second window the delayed ACK is counted along
        // with fresh re-ACKs from the dup retransmit.  Without this, every attempt
        // requires a full set of N-1 new ACKs from scratch.
        std::unordered_set<uint64_t> got;
        got.reserve(static_cast<size_t>(std::max(mesh_size, 1)) * 2);

        // Stop-and-wait: retry up to PEEL_DEFAULT_RETRIES times per packet
        bool acked = false;
        for (int attempt = 0; attempt < PEEL_DEFAULT_RETRIES && !acked; ++attempt) {
            if (!sendPacket(seq, flags, ptr, chunk)) return false;

            // No receivers in this mesh: nothing to wait for.
            if (mesh_size <= 1) { acked = true; break; }

            acked = waitForAcks(seq, config_.rto_ms, got);
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

    // Destination MAC: use the transport's fixed ruleset if configured,
    // otherwise derive the standard multicast MAC from the group IP.
    uint32_t mcast_ip = ch->mcast.sin_addr.s_addr;
    uint8_t  dst_mac[6];
    if (config_.use_cidr_rules_mac) {
        memcpy(dst_mac, config_.cidr_rules_mac, 6);
    } else {
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
        static_cast<uint8_t>(config_.dscp << 2),
        udp_payload.data(), udp_payload.size());

    if (flen == 0) return false;

    sockaddr_ll sll = ch->ll_dest;
    ssize_t n = sendto(ch->fd, frame.data(), flen, 0,
                       reinterpret_cast<const sockaddr*>(&sll), sizeof(sll));
    if (n != static_cast<ssize_t>(flen)) {
        const int saved_errno = errno;
        std::cerr << "peel_transport[" << config_.rank
                  << "]: sendto failed for seq=" << seq
                  << " frame_len=" << flen
                  << " payload_len=" << len
                  << " max_chunk_size=" << config_.max_chunk_size
                  << " iface=" << config_.iface_name;
        if (saved_errno != 0) {
            std::cerr << " errno=" << saved_errno
                      << " (" << std::strerror(saved_errno) << ")";
        }
        std::cerr << "\n";
        return false;
    }
    return true;
}

// =============================================================================
// Receive
// =============================================================================

ssize_t PeelTransport::recv(int from_rank, void* data, size_t max_size, int timeout_ms) {
    if (!isReady()) return -1;

    auto* ch = mesh_result_->getRecvChannel(from_rank);
    if (!ch || ch->fd < 0) return -1;

    auto* out = static_cast<uint8_t*>(data);
    size_t   received     = 0;
    bool     done         = false;
    bool     seq_init     = false;  // true once we've seen FLG_SYN for this broadcast
    uint32_t expected_seq = 0;      // next seq we expect from sender

    auto deadline = Clock::now() + std::chrono::milliseconds(
        timeout_ms > 0 ? timeout_ms : config_.timeout_ms);

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

        uint32_t pkt_seq = ntohl(hdr.seq);

        // ── Sequence deduplication ───────────────────────────────────────────
        // The sender uses stop-and-wait with retransmits.  If the same chunk
        // is retransmitted (its seq equals the one we already copied), blindly
        // appending it would corrupt the buffer and advance `received` past the
        // correct offset.  We use FLG_SYN to anchor the expected_seq at the
        // start of every broadcast, then accept only in-order chunks.
        //
        // For any out-of-order or duplicate packet we still send an ACK so the
        // sender does not keep retransmitting indefinitely.
        if (!seq_init) {
            if (!(flags & FLG_SYN)) {
                // Pre-broadcast stale packet from a previous iteration —
                // ACK it so the sender stops, but do not copy any data.
                sendAck(src_ip, ntohs(hdr.src_port), sender_mac,
                        pkt_seq, ntohl(hdr.tsval), hdr.retrans_id);
                continue;
            }
            expected_seq = pkt_seq;
            seq_init     = true;
        }

        if (pkt_seq != expected_seq) {
            // Retransmit of an already-received chunk, or a genuinely
            // out-of-order packet.  ACK it and skip the data copy.
            sendAck(src_ip, ntohs(hdr.src_port), sender_mac,
                    pkt_seq, ntohl(hdr.tsval), hdr.retrans_id);
            continue;
        }
        // ── end dedup ────────────────────────────────────────────────────────

        size_t app_len = plen - PEEL_HEADER_SIZE;
        if (received + app_len > max_size)
            app_len = max_size - received;

        if (app_len > 0) {
            std::memcpy(out + received, payload + PEEL_HEADER_SIZE, app_len);
            received += app_len;
        }

        // Send unicast ACK back to sender using learned MAC
        sendAck(src_ip, ntohs(hdr.src_port), sender_mac,
                pkt_seq, ntohl(hdr.tsval), hdr.retrans_id);

        if (flags & FLG_FIN) done = true;
        ++expected_seq;
    }

    // ==========================================================================
    // TIME_WAIT linger: re-ACK retransmitted FINs after the data phase ends.
    //
    // Problem: after the receiver copies the last chunk and sends the FIN-ACK it
    // exits this loop.  If that ACK is lost in transit, the sender retransmits
    // the FIN — but the receiver is no longer polling this socket, so no re-ACK
    // is ever sent.  The sender exhausts all PEEL_DEFAULT_RETRIES and fails.
    //
    // Fix: stay in a lightweight re-ACK loop for up to rto_ms milliseconds.  If
    // the sender's retransmit arrives in that window we respond immediately and
    // the sender succeeds on its next waitForAcks() call.  If no retransmit
    // arrives the linger exits cleanly with zero extra overhead per chunk.
    //
    // Note: this adds at most rto_ms latency per broadcast at the receiver side.
    // With rto_ms=500 ms and a reliable network the retransmit (if any) arrives
    // in < 1 ms, so in practice the linger exits on the very first poll.
    // ==========================================================================
    if (done) {
        uint32_t fin_seq = expected_seq - 1;  // seq of the FIN we just ACKed
        int linger_ms = config_.rto_ms > 0 ? config_.rto_ms : PEEL_DEFAULT_RTO_MS;
        auto linger_end = Clock::now() + std::chrono::milliseconds(linger_ms);

        timeval linger_tv{};
        linger_tv.tv_sec  = 0;
        linger_tv.tv_usec = 50000;  // 50 ms poll so we can check the deadline
        setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &linger_tv, sizeof(linger_tv));

        while (Clock::now() < linger_end) {
            uint8_t lbuf[65536];

            // Peek first so we do not accidentally consume the first packet of
            // the next transfer while lingering for a possible FIN retransmit.
            // If the peeked packet belongs to the next transfer, leave it queued
            // on the socket and let the next recv() call process it normally.
            ssize_t ln = ::recv(ch->fd, lbuf, sizeof(lbuf), MSG_PEEK);
            if (ln < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;  // no retransmit yet; keep waiting
                break;         // real socket error
            }

            uint8_t lmac[6]{};
            if (ln >= 12) memcpy(lmac, lbuf + 6, 6);

            uint32_t lsrc_ip; uint16_t lsrc_port; size_t lplen;
            const uint8_t* lpayload = parse_udp_frame(
                lbuf, ln,
                ch->mcast.sin_addr.s_addr,
                ch->port,
                lsrc_ip, lsrc_port, lplen);

            bool consume = true;
            bool reack_fin = false;

            if (lpayload && lplen >= PEEL_HEADER_SIZE) {
                PeelHeader lhdr{};
                std::memcpy(&lhdr, lpayload, sizeof(lhdr));
                if (peel_verify_header_checksum(lhdr) &&
                    lhdr.rank == static_cast<uint8_t>(from_rank) &&
                    (ntohs(lhdr.flags) & FLG_DATA)) {
                    const uint32_t lpkt_seq = ntohl(lhdr.seq);

                    if (lpkt_seq == fin_seq) {
                        // Sender is retransmitting the FIN — consume and re-ACK it.
                        reack_fin = true;
                    } else if (lpkt_seq > fin_seq) {
                        // First packet of the next transfer. Leave it queued so
                        // the next recv() call can consume it.
                        break;
                    }
                }
            }

            if (consume) {
                ln = ::recv(ch->fd, lbuf, sizeof(lbuf), 0);
                if (ln < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        continue;
                    break;
                }
            }

            if (reack_fin) {
                PeelHeader lhdr{};
                // Re-parse from the consumed frame so ACK fields are correct.
                uint32_t csrc_ip; uint16_t csrc_port; size_t cplen;
                const uint8_t* cpayload = parse_udp_frame(
                    lbuf, ln,
                    ch->mcast.sin_addr.s_addr,
                    ch->port,
                    csrc_ip, csrc_port, cplen);
                if (cpayload && cplen >= PEEL_HEADER_SIZE) {
                    std::memcpy(&lhdr, cpayload, sizeof(lhdr));
                    uint8_t cmac[6]{};
                    if (ln >= 12) memcpy(cmac, lbuf + 6, 6);
                    sendAck(csrc_ip, ntohs(lhdr.src_port), cmac,
                            ntohl(lhdr.seq), ntohl(lhdr.tsval), lhdr.retrans_id);
                }
            }
        }
        // Restore 100 ms poll timeout for next recv() call on this socket.
        timeval restore_tv{};
        restore_tv.tv_sec  = 0;
        restore_tv.tv_usec = 100000;
        setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &restore_tv, sizeof(restore_tv));
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
        0,   // TOS: no DSCP for unicast ACK
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

bool PeelTransport::waitForAcks(uint32_t seq, int timeout_ms,
                                  std::unordered_set<uint64_t>& got) {
    if (!isReady()) return false;

    auto* ch = mesh_result_->send_channel.get();
    if (!ch || ch->fd < 0) return false;

    int expected = config_.participant_ranks.empty()
        ? config_.world_size - 1
        : static_cast<int>(config_.participant_ranks.size()) - 1;
    if (expected <= 0) return true;
    // Short-circuit: caller accumulates got across retransmits; we may already
    // have enough ACKs from the previous attempt.
    if ((int)got.size() >= expected) return true;

    // Use a short poll interval so the deadline-check loop is reactive.
    // The old code set SO_RCVTIMEO = rto_ms and broke on EAGAIN — meaning the
    // entire rto_ms budget was spent on a single blocking recv() call, and any
    // ACK arriving epsilon after the timeout was silently discarded.  With 50 ms
    // poll intervals we check the deadline 10× per rto_ms and continue polling
    // instead of breaking, so late ACKs are still collected within the window.
    timeval poll_tv{};
    poll_tv.tv_sec  = 0;
    poll_tv.tv_usec = 50000;  // 50 ms poll interval
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &poll_tv, sizeof(poll_tv));

    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    while ((int)got.size() < expected && Clock::now() < deadline) {
        uint8_t buf[2048];
        ssize_t n = ::recv(ch->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;  // keep polling until deadline
            break;         // real socket error
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

        // Unique key per receiver: pack (src_ip, src_port) into 64 bits.
        // src_ip occupies bits 47-16, src_port occupies bits 15-0 — no overlap.
        uint64_t key = ((uint64_t)(uint32_t)src_ip << 16) | src_port;
        got.insert(key);
    }

    // Restore the original SO_RCVTIMEO (used by performHandshake on the same fd).
    timeval orig_tv{};
    orig_tv.tv_sec  = config_.rto_ms / 1000;
    orig_tv.tv_usec = (config_.rto_ms % 1000) * 1000;
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &orig_tv, sizeof(orig_tv));

    return (int)got.size() >= expected;
}

} // namespace peel
} // namespace transport
} // namespace gloo
