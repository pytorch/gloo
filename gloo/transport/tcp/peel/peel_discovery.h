// gloo/transport/tcp/peel/peel_discovery.h

#pragma once

#include "peel_protocol.h"

#include <string>
#include <unordered_map>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

struct PeelDiscoveryConfig {
    int         rank             = 0;
    int         world_size       = 1;
    std::string iface_name;                          // NIC name, e.g. "eth0"
    std::string redis_host       = "127.0.0.1";
    int         redis_port       = 6379;
    std::string redis_prefix     = "peel";           // keys: <prefix>/ip/<rank>
    int         timeout_ms       = PEEL_DEFAULT_TIMEOUT_MS;
    int         poll_interval_ms = 50;
};

// Lightweight Redis-only phase: every rank publishes its unicast IP and
// waits until every other rank has done the same.
// Result: a complete rank -> IP map (dotted-decimal strings) available to
// all ranks before any sockets or transports are created.
class PeelDiscovery {
public:
    explicit PeelDiscovery(const PeelDiscoveryConfig& config);
    ~PeelDiscovery();

    // Publish own IP to Redis, wait for all peers, collect all IPs.
    // Returns true on success.
    bool run();

    // Dotted-decimal IP string for the given rank.
    // Returns "" if run() has not been called or rank is out of range.
    std::string getIp(int rank) const;

    // This rank's own dotted-decimal IP string.
    const std::string& myIp() const { return my_ip_str_; }

    // Full map: rank -> dotted-decimal IP string.
    const std::unordered_map<int, std::string>& peerIps() const { return peer_ips_; }

private:
    PeelDiscoveryConfig                    config_;
    std::string                            my_ip_str_;
    std::string                            my_key_;     // Redis key for this rank's IP
    std::unordered_map<int, std::string>   peer_ips_;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
