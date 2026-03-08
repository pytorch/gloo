#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declare hiredis types to avoid header dependency
struct redisContext;

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

class PeelRedis {
public:
    PeelRedis(const std::string& host, int port = 6379);
    ~PeelRedis();

    PeelRedis(const PeelRedis&) = delete;
    PeelRedis& operator=(const PeelRedis&) = delete;

    // Connect to Redis server
    bool connect();
    
    // Disconnect from Redis server
    void disconnect();
    
    // Check if connected
    bool isConnected() const;

    // Set a key-value pair
    bool set(const std::string& key, const std::string& value);
    
    // Get a value by key (returns empty string if not found)
    std::string get(const std::string& key);
    
    // Delete a key
    bool del(const std::string& key);
    
    // Delete all keys matching a pattern (e.g., "peel/*")
    int delPattern(const std::string& pattern);
    
    // Check if key exists
    bool exists(const std::string& key);
    
    // Wait for a key to exist (non-blocking internally, polls with sleep)
    // Returns true if key found, false if timeout
    bool waitForKey(const std::string& key, int timeout_ms, int poll_interval_ms = 50);
    
    // Wait for multiple keys to exist
    // Returns true if all keys found, false if timeout
    bool waitForKeys(const std::vector<std::string>& keys, int timeout_ms, int poll_interval_ms = 50);
    
    // Barrier: wait for N ranks to set their ready key
    // Keys are formatted as: prefix + "/" + rank_id
    bool barrier(const std::string& prefix, int world_size, int timeout_ms);

private:
    std::string host_;
    int port_;
    redisContext* ctx_ = nullptr;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo