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

bool PeelRedis::reconnect() {
    disconnect();
    return connect();
}

bool PeelRedis::set(const std::string& key, const std::string& value) {
    // Retry up to 3 times.  On any anomalous reply (NULL reply, STATUS with
    // null string, or GET-verification mismatch) we:
    //   1. DEL the key first — clears any stale value from a previous crashed run
    //      that would make a fresh SET look identical to a no-op.
    //   2. Disconnect and reconnect — resets the hiredis context in case the
    //      connection entered a broken state (seen on cross-server paths where
    //      TCP segmentation causes hiredis to receive a truncated reply and
    //      return REDIS_REPLY_STATUS with str==NULL).
    //   3. Retry the SET.
    constexpr int kMaxAttempts = 3;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // Ensure we have a live connection before issuing commands.
        if (!ctx_ && !connect()) {
            std::cerr << "peel_redis: connect failed on attempt " << attempt
                      << " for key: " << key << "\n";
            continue;
        }

        // On retries, explicitly delete any stale key first so the subsequent
        // SET writes a fresh value regardless of what was there before.
        if (attempt > 1) {
            auto* del_reply = static_cast<redisReply*>(
                redisCommand(ctx_, "DEL %s", key.c_str()));
            if (del_reply) freeReplyObject(del_reply);
        }

        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx_, "SET %s %s", key.c_str(), value.c_str()));

        if (!reply) {
            // NULL reply means the hiredis context is in an error state.
            std::cerr << "peel_redis: SET returned NULL reply"
                      << (ctx_ ? std::string(" (ctx err=") + std::to_string(ctx_->err)
                                    + " '" + ctx_->errstr + "')" : "")
                      << " for key: " << key
                      << " (attempt " << attempt << "/" << kMaxAttempts << ")\n";
            reconnect();
            continue;
        }

        bool ok = false;
        if (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr) {
            ok = (std::strcmp(reply->str, "OK") == 0);
            if (!ok)
                std::cerr << "peel_redis: SET status not OK: '" << reply->str
                          << "' for key: " << key
                          << " (attempt " << attempt << "/" << kMaxAttempts << ")\n";
        } else if (reply->type == REDIS_REPLY_STATUS && reply->str == nullptr) {
            // Abnormal: STATUS reply with no string body.  This has been observed
            // on cross-server paths where a TCP-segmented reply arrives incomplete.
            // Treat as a soft error and retry after reconnect.
            std::cerr << "peel_redis: SET reply->str is NULL (type=STATUS) for key: "
                      << key << " (attempt " << attempt << "/" << kMaxAttempts
                      << ") — reconnecting and retrying\n";
            freeReplyObject(reply);
            reconnect();
            continue;
        } else if (reply->type == REDIS_REPLY_ERROR) {
            std::cerr << "peel_redis: SET error: "
                      << (reply->str ? reply->str : "unknown")
                      << " for key: " << key
                      << " (attempt " << attempt << "/" << kMaxAttempts << ")\n";
        } else {
            std::cerr << "peel_redis: SET unexpected reply type " << reply->type
                      << " for key: " << key
                      << " (attempt " << attempt << "/" << kMaxAttempts << ")\n";
        }
        freeReplyObject(reply);

        if (!ok) {
            reconnect();
            continue;
        }

        // Verify the value was actually stored.  Guards against the case where
        // SET returned OK but the connection was silently broken mid-write and
        // the value on the server side is still stale (or absent).
        const std::string stored = get(key);
        if (stored != value) {
            std::cerr << "peel_redis: SET verification failed for key: " << key
                      << " (expected '" << value << "', got '" << stored << "')"
                      << " (attempt " << attempt << "/" << kMaxAttempts << ")\n";
            reconnect();
            continue;
        }

        return true;  // SET confirmed successful
    }

    std::cerr << "peel_redis: SET failed after " << kMaxAttempts
              << " attempt(s) for key: " << key << "\n";
    return false;
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