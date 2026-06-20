#include "peel_allgather_ring.h"

#include <iostream>

namespace gloo {
namespace transport {
namespace peel {

PeelAllgatherRing::PeelAllgatherRing(int rank, std::vector<PeelRingHop> hops)
    : rank_(rank), hops_(std::move(hops)) {}

bool PeelAllgatherRing::run(const std::vector<void*>& bufs, size_t size) {
  const int worldSize = static_cast<int>(hops_.size());

  if (worldSize <= 1 || size == 0) {
    return true;
  }
  if (rank_ < 0 || rank_ >= worldSize) {
    std::cerr << "peel_allgather_ring: invalid local rank " << rank_
              << " for world size " << worldSize << "\n";
    return false;
  }
  if (static_cast<int>(bufs.size()) != worldSize) {
    std::cerr << "peel_allgather_ring: bufs.size()=" << bufs.size()
              << " but world size is " << worldSize << "\n";
    return false;
  }
  for (int src = 0; src < worldSize; ++src) {
    if (bufs[src] == nullptr) {
      std::cerr << "peel_allgather_ring: null buffer for source rank " << src
                << "\n";
      return false;
    }
  }

  const int sendHopIdx = rank_;
  const int recvHopIdx = (rank_ + worldSize - 1) % worldSize;
  auto* sendTransport = hops_[sendHopIdx].transport;
  auto* recvTransport = hops_[recvHopIdx].transport;

  for (int step = 0; step < worldSize - 1; ++step) {
    const int sendSrc = (rank_ - step + worldSize) % worldSize;
    const int recvSrc = (rank_ - step - 1 + worldSize) % worldSize;

    bool postedRecv = false;
    bool postedSend = false;

    if (recvTransport != nullptr) {
      if (!recvTransport->isReady()) {
        std::cerr << "peel_allgather_ring: recv hop transport not ready: "
                  << hops_[recvHopIdx].sender << " -> "
                  << hops_[recvHopIdx].receiver << "\n";
        return false;
      }
      recvTransport->submitWork(hops_[recvHopIdx].sender, bufs[recvSrc], size);
      postedRecv = true;
    }

    if (sendTransport != nullptr) {
      if (!sendTransport->isReady()) {
        std::cerr << "peel_allgather_ring: send hop transport not ready: "
                  << hops_[sendHopIdx].sender << " -> "
                  << hops_[sendHopIdx].receiver << "\n";
        return false;
      }
      sendTransport->submitWork(hops_[sendHopIdx].sender, bufs[sendSrc], size);
      postedSend = true;
    }

    if (postedSend && !sendTransport->waitResult()) {
      std::cerr << "peel_allgather_ring: send hop failed in round " << step
                << ": " << hops_[sendHopIdx].sender << " -> "
                << hops_[sendHopIdx].receiver << " carrying src=" << sendSrc
                << "\n";
      return false;
    }
    if (postedRecv && !recvTransport->waitResult()) {
      std::cerr << "peel_allgather_ring: recv hop failed in round " << step
                << ": " << hops_[recvHopIdx].sender << " -> "
                << hops_[recvHopIdx].receiver << " carrying src=" << recvSrc
                << "\n";
      return false;
    }
  }

  return true;
}

} // namespace peel
} // namespace transport
} // namespace gloo
