// gloo/transport/tcp/peel/peel_transport.h

#pragma once

#include "peel_full_mesh.h"
#include "peel_protocol.h"

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// =============================================================================
// Transport Configuration
// =============================================================================

struct PeelTransportConfig {
    // From PeelFullMeshConfig
    std::string mcast_group = "239.255.0.1";
    uint16_t base_port = PEEL_DEFAULT_BASE_PORT;
    int rank = 0;
    int world_size = 1;
    std::string iface_name;
    int ttl = PEEL_DEFAULT_TTL;
    int rcvbuf = 4 * 1024 * 1024;
    int rto_ms = PEEL_DEFAULT_RTO_MS;
    int timeout_ms = PEEL_DEFAULT_TIMEOUT_MS;

    // Subset of ranks in this transport's mesh (matches PeelSubtree::receiver_ranks).
    // Empty means all ranks 0..world_size-1.
    std::vector<int> participant_ranks;

    // Data transfer
    size_t max_chunk_size = PEEL_MAX_PAYLOAD;

    // Routing ruleset: if use_cidr_rules_mac is true, every outgoing DATA frame
    // uses cidr_rules_mac as the Ethernet destination instead of the standard
    // derived multicast MAC.
    uint8_t cidr_rules_mac[6]  = {};
    bool    use_cidr_rules_mac = false;

    // DSCP for outgoing multicast packets (written to ip->tos as dscp << 2).
    uint8_t dscp = 7;

    // Rank that sends data in this mesh (broadcast root).
    // Passed through to PeelFullMeshConfig to drive the handshake direction.
    int sender_rank = 0;
};

// =============================================================================
// Peel Transport
// =============================================================================

class PeelTransport {
public:
    explicit PeelTransport(const PeelTransportConfig& config);
    ~PeelTransport();

    PeelTransport(const PeelTransport&) = delete;
    PeelTransport& operator=(const PeelTransport&) = delete;

    // Initialize and perform handshake
    bool init();

    // Check if ready
    bool isReady() const { return mesh_result_ != nullptr; }

    // Get identity
    int rank() const { return config_.rank; }
    int worldSize() const { return config_.world_size; }

    // =========================================================================
    // Broadcast API
    // =========================================================================

    // Broadcast data from root to all other ranks.
    // Convenience wrapper: submits work to the worker thread and blocks
    // until it completes. Equivalent to submitWork() + waitResult().
    bool broadcast(int root, void* data, size_t size);

    // Submit a broadcast operation to the worker thread (non-blocking).
    // The caller must ensure the worker is IDLE before calling.
    // Pair with waitResult() to retrieve the outcome.
    // The data buffer must remain valid until waitResult() returns.
    void submitWork(int root, void* data, size_t size);

    // Block until the worker thread finishes the submitted operation.
    // Returns the result and transitions the worker back to IDLE.
    bool waitResult();

    // =========================================================================
    // Low-level Send/Recv (for future extensions)
    // =========================================================================

    // Send data on this rank's multicast channel (to all)
    bool send(const void* data, size_t size);

    // Receive data from a specific rank
    // timeout_ms: -1 = blocking, 0 = non-blocking, >0 = timeout
    // Returns bytes received, 0 on timeout, -1 on error
    ssize_t recv(int from_rank, void* data, size_t max_size, int timeout_ms = -1);

    // =========================================================================
    // Cleanup
    // =========================================================================

    void cleanup();

private:
    // Send a single packet
    bool sendPacket(uint32_t seq, uint16_t flags, const void* payload, size_t len);

    // Receive a packet from specific rank
    bool recvPacket(int from_rank, PeelHeader& hdr,
                    std::vector<uint8_t>& payload, int timeout_ms);

    // Wait for ACKs from all receivers (stop-and-wait)
    bool waitForAcks(uint32_t seq, int timeout_ms);

    // Send a unicast ACK frame back to the sender
    void sendAck(uint32_t dst_ip_n, uint16_t dst_port_h, const uint8_t dst_mac[6],
                 uint32_t seq, uint32_t tsecr, uint8_t retrans_id);

    // =========================================================================
    // Worker thread
    // =========================================================================

    // States of the worker thread state machine.
    //   IDLE     — waiting for the next submitWork() call.
    //   WORKING  — executing a broadcast operation.
    //   DONE     — operation complete, result available for waitResult().
    //   SHUTDOWN — cleanup() has been called; thread will exit.
    enum class WorkerState { IDLE, WORKING, DONE, SHUTDOWN };

    // Parameters for one broadcast operation, written by submitWork()
    // and read by the worker thread under mu_.
    struct WorkItem {
        int    root = 0;
        void*  data = nullptr;
        size_t size = 0;
    };

    // Execute one broadcast operation. Called exclusively from workerLoop().
    bool executeBroadcast(int root, void* data, size_t size);

    // Worker thread entry point.
    void workerLoop();

    PeelTransportConfig                  config_;
    std::unique_ptr<PeelFullMeshResult>  mesh_result_;
    uint32_t                             next_seq_ = 1;
    uint16_t                             ip_id_    = 0;

    std::thread             worker_;
    std::mutex              mu_;
    std::condition_variable cv_work_;              // main  → worker : WORKING or SHUTDOWN
    std::condition_variable cv_done_;              // worker → main  : DONE
    WorkerState             worker_state_{WorkerState::IDLE};
    WorkItem                work_item_{};
    bool                    work_result_{false};
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo