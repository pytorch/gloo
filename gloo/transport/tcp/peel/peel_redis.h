#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

    bool connect();
    void disconnect();
    bool isConnected() const;

    bool set(const std::string& key, const std::string& value);
    std::string get(const std::string& key);
    bool del(const std::string& key);
    int delPattern(const std::string& pattern);
    bool exists(const std::string& key);

    bool waitForKey(const std::string& key, int timeout_ms, int poll_ms = 50);
    bool waitForKeys(const std::vector<std::string>& keys, int timeout_ms, int poll_ms = 50);

private:
    std::string host_;
    int port_;
    redisContext* ctx_ = nullptr;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo