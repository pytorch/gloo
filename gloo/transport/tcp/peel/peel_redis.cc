#include "peel_redis.h"

#include <hiredis/hiredis.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

using Clock = std::chrono::steady_clock;

PeelRedis::PeelRedis(const std::string& host, int port)
    : host_(host), port_(port) {}

PeelRedis::~PeelRedis() {
    disconnect();
}

bool PeelRedis::connect() {
    if (ctx_) return true;

    timeval timeout = {5, 0};
    ctx_ = redisConnectWithTimeout(host_.c_str(), port_, timeout);

    if (!ctx_) {
        std::cerr << "peel_redis: allocation failed\n";
        return false;
    }
    if (ctx_->err) {
        std::cerr << "peel_redis: connect error: " << ctx_->errstr << "\n";
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }

    std::cerr << "peel_redis: connected to " << host_ << ":" << port_ << "\n";
    return true;
}

void PeelRedis::disconnect() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool PeelRedis::isConnected() const {
    return ctx_ && ctx_->err == 0;
}

bool PeelRedis::set(const std::string& key, const std::string& value) {
    if (!ctx_) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SET %s %s", key.c_str(), value.c_str()));
    if (!reply) return false;

    bool ok = false;
    if (reply->str != nullptr) {
        ok = (reply->type == REDIS_REPLY_STATUS &&
              std::strcmp(reply->str, "OK") == 0);
    } else if (reply->type == REDIS_REPLY_STATUS) {
        ok = true;
        std::cerr << "Warning: Redis SET reply->str is NULL, but type is STATUS" << std::endl;
    } else if (reply->type == REDIS_REPLY_ERROR) {
        std::cerr << "Redis SET error: " << (reply->str ? reply->str : "unknown error") 
                  << " for key: " << key << std::endl;
    }
    
    freeReplyObject(reply);
    return ok;
}

std::string PeelRedis::get(const std::string& key) {
    if (!ctx_) return "";

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", key.c_str()));
    if (!reply) return "";

    std::string result;
    if (reply->type == REDIS_REPLY_STRING && reply->str) {
        result = reply->str;
    }
    freeReplyObject(reply);
    return result;
}

bool PeelRedis::del(const std::string& key) {
    if (!ctx_) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "DEL %s", key.c_str()));
    if (!reply) return false;

    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return ok;
}

int PeelRedis::delPattern(const std::string& pattern) {
    if (!ctx_) return -1;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "KEYS %s", pattern.c_str()));
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return -1;
    }

    int deleted = 0;
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type == REDIS_REPLY_STRING) {
            if (del(reply->element[i]->str)) ++deleted;
        }
    }
    freeReplyObject(reply);
    return deleted;
}

bool PeelRedis::exists(const std::string& key) {
    if (!ctx_) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "EXISTS %s", key.c_str()));
    if (!reply) return false;

    bool found = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return found;
}

bool PeelRedis::waitForKey(const std::string& key, int timeout_ms, int poll_ms) {
    auto start = Clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    auto poll = std::chrono::milliseconds(poll_ms);

    while (Clock::now() - start < timeout) {
        if (exists(key)) return true;
        std::this_thread::sleep_for(poll);
    }
    return false;
}

bool PeelRedis::waitForKeys(const std::vector<std::string>& keys,
                            int timeout_ms, int poll_ms) {
    auto start = Clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    auto poll = std::chrono::milliseconds(poll_ms);

    std::vector<bool> found(keys.size(), false);
    size_t count = 0;

    while (Clock::now() - start < timeout && count < keys.size()) {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!found[i] && exists(keys[i])) {
                found[i] = true;
                ++count;
            }
        }
        if (count < keys.size()) {
            std::this_thread::sleep_for(poll);
        }
    }
    return count == keys.size();
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
