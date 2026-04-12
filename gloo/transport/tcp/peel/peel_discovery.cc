// gloo/transport/tcp/peel/peel_discovery.cc

#include "peel_discovery.h"
#include "peel_redis.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// ioctl helper: returns this NIC's unicast IP as a dotted-decimal string.
static bool discovery_get_iface_ip(const std::string& iface, std::string& ip_str) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket(discovery_get_iface_ip)"); return false; }

    ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
        perror(("SIOCGIFADDR " + iface).c_str());
        ::close(s);
        return false;
    }
    ::close(s);

    char buf[INET_ADDRSTRLEN];
    auto* sin = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    ip_str = buf;
    return true;
}

// =============================================================================

PeelDiscovery::PeelDiscovery(const PeelDiscoveryConfig& config)
    : config_(config) {}

PeelDiscovery::~PeelDiscovery() {
    if (my_key_.empty()) return;  // run() was never called, nothing to clean up
    PeelRedis redis(config_.redis_host, config_.redis_port);
    if (redis.connect())
        redis.del(my_key_);
}

bool PeelDiscovery::run() {
    // ------------------------------------------------------------------
    // Step 1: derive own unicast IP from NIC as a human-readable string
    // ------------------------------------------------------------------
    std::string my_ip_str;
    if (!discovery_get_iface_ip(config_.iface_name, my_ip_str)) {
        std::cerr << "peel_discovery[" << config_.rank
                  << "]: failed to get IP for iface '" << config_.iface_name << "'\n";
        return false;
    }
    my_ip_str_ = my_ip_str;

    std::cout << "peel_discovery[" << config_.rank
              << "]: my IP = " << my_ip_str << "\n";

    // ------------------------------------------------------------------
    // Step 2: connect to Redis
    // ------------------------------------------------------------------
    PeelRedis redis(config_.redis_host, config_.redis_port);
    if (!redis.connect()) {
        std::cerr << "peel_discovery[" << config_.rank
                  << "]: Redis connect failed ("
                  << config_.redis_host << ":" << config_.redis_port << ")\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Step 3: publish own IP
    //   key:   <prefix>/ip/<rank>
    //   value: "10.0.1.5"  (dotted-decimal string)
    // ------------------------------------------------------------------
    my_key_ = config_.redis_prefix + "/ip/" + std::to_string(config_.rank);
    if (!redis.set(my_key_, my_ip_str)) {
        std::cerr << "peel_discovery[" << config_.rank
                  << "]: Redis SET failed for key " << my_key_ << "\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Step 4+5: wait until every rank has published a non-empty IP and
    //           collect all values in one pass.
    //
    // We poll get() rather than exists() because exists() returns true
    // even for stale keys with empty values left by a previous crashed run.
    // A key is only "ready" once it holds a valid dotted-decimal IP string.
    // ------------------------------------------------------------------
    std::vector<std::string> all_keys;
    all_keys.reserve(config_.world_size);
    for (int r = 0; r < config_.world_size; ++r)
        all_keys.push_back(config_.redis_prefix + "/ip/" + std::to_string(r));

    {
        using Clock = std::chrono::steady_clock;
        auto deadline = Clock::now() +
                        std::chrono::milliseconds(config_.timeout_ms);
        auto poll_dur = std::chrono::milliseconds(config_.poll_interval_ms);

        std::vector<std::string> collected(config_.world_size);
        std::vector<bool>        ready(config_.world_size, false);
        int                      ready_count = 0;

        while (ready_count < config_.world_size && Clock::now() < deadline) {
            for (int r = 0; r < config_.world_size; ++r) {
                if (ready[r]) continue;
                std::string val = redis.get(all_keys[r]);
                if (!val.empty()) {
                    collected[r] = val;
                    ready[r]     = true;
                    ++ready_count;
                }
            }
            if (ready_count < config_.world_size)
                std::this_thread::sleep_for(poll_dur);
        }

        if (ready_count < config_.world_size) {
            std::cerr << "peel_discovery[" << config_.rank
                      << "]: timeout waiting for all ranks to publish IPs"
                         " (" << ready_count << "/" << config_.world_size
                      << " ready)\n";
            return false;
        }

        for (int r = 0; r < config_.world_size; ++r) {
            peer_ips_[r] = collected[r];
            std::cout << "peel_discovery[" << config_.rank
                      << "]:   rank " << r << " -> " << collected[r] << "\n";
        }
    }

    std::cout << "peel_discovery[" << config_.rank
              << "]: complete (" << peer_ips_.size() << " ranks)\n";
    return true;
}

std::string PeelDiscovery::getIp(int rank) const {
    auto it = peer_ips_.find(rank);
    return (it != peer_ips_.end()) ? it->second : "";
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
