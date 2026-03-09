// gloo/transport/tcp/peel/test_peel_gloo_context.cc

#include "peel_context.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace gloo::transport::tcp::peel;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <rank> <world_size> [redis_host]\n";
        return 1;
    }

    int rank = std::atoi(argv[1]);
    int world_size = std::atoi(argv[2]);
    std::string redis_host = argc > 3 ? argv[3] : "127.0.0.1";

    PeelContextConfig config;
    config.rank = rank;
    config.world_size = world_size;
    config.redis_host = redis_host;
    config.mcast_group = "239.255.0.1";
    config.base_port = 50000;

    std::cerr << "=== Test PeelContext Broadcast ===\n";
    std::cerr << "rank=" << rank << ", world_size=" << world_size << "\n";

    PeelContext ctx(config);

    if (!ctx.init()) {
        std::cerr << "FAIL: init()\n";
        return 2;
    }

    // Test broadcast from rank 0
    constexpr size_t kSize = 1024;
    std::vector<uint32_t> data(kSize);
    int root = 0;

    if (rank == root) {
        for (size_t i = 0; i < kSize; ++i) {
            data[i] = static_cast<uint32_t>(i * 42 + 7);
        }
        std::cerr << "Rank " << rank << ": broadcasting " << kSize << " uint32s\n";
    } else {
        std::memset(data.data(), 0, data.size() * sizeof(uint32_t));
    }

    if (!ctx.broadcast(root, data.data(), data.size() * sizeof(uint32_t))) {
        std::cerr << "FAIL: broadcast()\n";
        return 3;
    }

    // Verify
    bool ok = true;
    for (size_t i = 0; i < kSize; ++i) {
        uint32_t expected = static_cast<uint32_t>(i * 42 + 7);
        if (data[i] != expected) {
            std::cerr << "FAIL: data[" << i << "] = " << data[i]
                      << ", expected " << expected << "\n";
            ok = false;
            break;
        }
    }

    if (ok) {
        std::cerr << "SUCCESS: rank " << rank << " verified broadcast\n";
    }

    ctx.cleanup();
    return ok ? 0 : 4;
}