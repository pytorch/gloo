#include "peel_full_mesh.h"

#include <cstdlib>
#include <iostream>

using namespace gloo::transport::tcp::peel;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <rank> <world_size> [redis_host]\n";
        return 1;
    }

    int rank = std::atoi(argv[1]);
    int world_size = std::atoi(argv[2]);
    std::string redis_host = argc > 3 ? argv[3] : "127.0.0.1";

    PeelFullMeshConfig config;
    config.rank = rank;
    config.world_size = world_size;
    config.redis_host = redis_host;
    config.mcast_group = "239.255.0.1";
    config.base_port = 50000;

    std::cerr << "=== Test PeelFullMesh ===\n";
    std::cerr << "rank=" << rank << ", world_size=" << world_size
              << ", redis=" << redis_host << "\n";

    PeelFullMesh mesh(config);

    if (!mesh.init()) {
        std::cerr << "FAIL: init()\n";
        return 2;
    }

    auto result = mesh.run();
    if (!result) {
        std::cerr << "FAIL: run()\n";
        return 3;
    }

    std::cerr << "SUCCESS: handshake complete\n";
    std::cerr << "  send_channel: port=" << result->send_channel->port
              << ", fd=" << result->send_channel->fd << "\n";
    std::cerr << "  recv_channels: " << result->recv_channels.size() << "\n";

    for (const auto& ch : result->recv_channels) {
        std::cerr << "    from rank " << ch->owner_rank
                  << ": port=" << ch->port << ", fd=" << ch->fd << "\n";
    }

    // Cleanup (only rank 0)
    if (rank == 0) {
        mesh.cleanup();
    }

    return 0;
}