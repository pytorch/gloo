// gloo/transport/peel/peel_context.h

#pragma once

#include "peel_broadcast.h"
#include "peel_transport.h"
#include "peel_broadcast_ring.h"
#include "peel_broadcast_stop_and_wait.h"
#include "peel_allgather_ring.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gloo {
namespace transport {
namespace peel {

// Configuration for Peel-enabled context.
struct PeelContextConfig {
    // Identity
    int rank       = 0;
    int world_size = 1;

    // Unicast IP for every rank, populated by PeelDiscovery before init().
    // Key = rank, Value = dotted-decimal string (e.g. "10.0.0.1").
    // When non-empty and topology_file is also set, PeelTree is used to build
    // the multicast spanning tree and partition it into subtrees.
    std::unordered_map<int, std::string> peer_ips;

    // Path to the topology adjacency file.  Empty = no topology, fall back to
    // a single flat transport covering all ranks (pre-PeelTree behaviour).
    std::string topology_file;

    // Multicast
    std::string mcast_group = "239.255.0.1";
    uint16_t    base_port   = PEEL_DEFAULT_BASE_PORT;
    std::string iface_name;
    int         ttl         = PEEL_DEFAULT_TTL;

    // DSCP for outgoing multicast packets (written to ip->tos as dscp << 2).
    uint8_t dscp = 7;

    // Rank that is the data sender (broadcast root).
    // All subtree transports are configured with this value.
    int sender_rank = 0;

    // Timing
    int timeout_ms = PEEL_DEFAULT_TIMEOUT_MS;
    int rto_ms     = PEEL_DEFAULT_RTO_MS;

    // Buffer
    int    rcvbuf         = 32 * 1024 * 1024;
    size_t max_chunk_size = 0;
};

// Owns all subtree transports and the PeelBroadcast that orchestrates them.
// Callers above this layer (PeelContext::broadcast, context.h::peelBroadcast)
// are unchanged — the parallelism is entirely internal.
class PeelContext {
public:
    explicit PeelContext(const PeelContextConfig& config);
    ~PeelContext();

    PeelContext(const PeelContext&) = delete;
    PeelContext& operator=(const PeelContext&) = delete;

    // Initialize all transports and wire up PeelBroadcast.
    // When config_.topology_file is non-empty and config_.peer_ips is
    // populated, builds a PeelTree and creates one transport per subtree.
    // Otherwise falls back to a single flat transport (all ranks, no CIDR).
    bool init();

    // True when all transports are ready.
    bool isReady() const;

    // Broadcast via PeelBroadcast (parallel across all subtree transports).
    bool broadcast(int root, void* data, size_t size);
	
	bool broadcastRing(int root, void* data, size_t size);
    bool broadcastStopAndWait(int root, void* data, size_t size);
    bool allgatherRing(const std::vector<void*>& bufs, size_t size);
    bool initRing();
    bool initStopAndWait();

    const std::vector<PeelRingHop>& ringHops() const { return ring_hops_; }

    // Cleanup all transports.
    void cleanup();

    // Accessors
    int rank()      const { return config_.rank; }
    int worldSize() const { return config_.world_size; }

private:
    // Fallback path: one transport, all ranks, no CIDR rules.
    // Used when no topology file is provided or peer_ips is empty.
    bool initSingleTransport();

    PeelContextConfig                           config_;
    std::vector<std::unique_ptr<PeelTransport>> transports_;
    std::unique_ptr<PeelBroadcast>              broadcast_;
    std::vector<std::unique_ptr<PeelTransport>> ring_transports_;
    std::vector<PeelRingHop>                    ring_hops_;
    std::unique_ptr<PeelBroadcastRing>          broadcast_ring_;
    std::vector<std::unique_ptr<PeelTransport>> stop_and_wait_transports_;
    std::unique_ptr<PeelBroadcastStopAndWait>   broadcast_stop_and_wait_;
    std::unique_ptr<PeelAllgatherRing>          allgather_ring_;
};

} // namespace peel
} // namespace transport
} // namespace gloo
