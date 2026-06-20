#include "peel_full_mesh.h"

#include <cstdlib>
#include <iostream>

using namespace gloo::transport::peel;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <rank> <world_size> [iface]\n";
        return 1;
    }

    int rank       = std::atoi(argv[1]);
    int world_size = std::atoi(argv[2]);
    std::string iface      = argc > 3 ? argv[3] : "";

    PeelFullMeshConfig config;
    config.rank        = rank;
    config.world_size  = world_size;
    config.mcast_group = "239.255.0.1";
    config.base_port   = 50000;
    if (!iface.empty())
        config.iface_name = iface;

    std::cerr << "=== Test PeelFullMesh ===\n";
    std::cerr << "rank=" << rank << ", world_size=" << world_size
              << ", iface=" << iface << "\n";

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

    return 0;
}