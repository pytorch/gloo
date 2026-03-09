#include "peel_context.h"

#include <cstdlib>
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

    PeelContext ctx(config);

    if (!ctx.init()) {
        std::cerr << "Failed to initialize peel context\n";
        return 2;
    }

    // Test broadcast
    std::vector<int> data(1024);
    int root = 0;

    if (rank == root) {
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<int>(i * 100);
        }
        std::cerr << "Rank " << rank << ": broadcasting data\n";
    }

    if (!ctx.broadcast(root, data.data(), data.size() * sizeof(int))) {
        std::cerr << "Rank " << rank << ": broadcast failed\n";
        return 3;
    }

    // Verify
    bool ok = true;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != static_cast<int>(i * 100)) {
            std::cerr << "Rank " << rank << ": mismatch at " << i
                      << ": got " << data[i] << "\n";
            ok = false;
            break;
        }
    }

    if (ok) {
        std::cerr << "Rank " << rank << ": broadcast SUCCESS\n";
    }

    ctx.cleanup();
    return ok ? 0 : 4;
}