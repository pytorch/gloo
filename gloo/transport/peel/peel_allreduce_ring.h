#pragma once

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "gloo/algorithm.h"
#include "gloo/transport/peel/peel_broadcast_ring.h"

namespace gloo {
namespace transport {
namespace peel {

// Ring allreduce over the same 2-rank Peel hop transports used by
// PeelBroadcastRing / PeelAllgatherRing.
//
// This mirrors the dataflow of gloo/allreduce_ring.h:
// - locally reduce all input pointers into ptrs_[0]
// - circulate the rolling partial reduction around the ring
// - accumulate every received partial into ptrs_[0]
// - copy the final result back into the remaining local pointers
template <typename T>
class PeelAllreduceRing {
 public:
  PeelAllreduceRing(
      int rank,
      std::vector<PeelRingHop> hops,
      const std::vector<T*>& ptrs,
      int count,
      const ReductionFunction<T>* fn = ReductionFunction<T>::sum)
      : rank_(rank),
        hops_(std::move(hops)),
        ptrs_(ptrs),
        count_(count),
        bytes_(count_ * sizeof(T)),
        fn_(fn) {
    inbox_ = static_cast<T*>(std::malloc(bytes_));
    outbox_ = static_cast<T*>(std::malloc(bytes_));
  }

  ~PeelAllreduceRing() {
    if (inbox_ != nullptr) {
      std::free(inbox_);
    }
    if (outbox_ != nullptr) {
      std::free(outbox_);
    }
  }

  bool run() {
    if (ptrs_.empty()) {
      std::cerr << "peel_allreduce_ring: no input pointers\n";
      return false;
    }

    if (count_ == 0) {
      return true;
    }

    for (size_t i = 1; i < ptrs_.size(); ++i) {
      fn_->call(ptrs_[0], ptrs_[i], count_);
    }

    const int worldSize = static_cast<int>(hops_.size());
    if (worldSize <= 1) {
      for (size_t i = 1; i < ptrs_.size(); ++i) {
        std::memcpy(ptrs_[i], ptrs_[0], bytes_);
      }
      return true;
    }

    if (rank_ < 0 || rank_ >= worldSize) {
      std::cerr << "peel_allreduce_ring: invalid local rank " << rank_
                << " for world size " << worldSize << "\n";
      return false;
    }

    const int sendHopIdx = rank_;
    const int recvHopIdx = (rank_ + worldSize - 1) % worldSize;

    auto* sendTransport = hops_[sendHopIdx].transport;
    auto* recvTransport = hops_[recvHopIdx].transport;

    if (sendTransport == nullptr || recvTransport == nullptr) {
      std::cerr << "peel_allreduce_ring: missing local hop transport(s) for rank "
                << rank_ << " sendHop=" << sendHopIdx
                << " recvHop=" << recvHopIdx << "\n";
      return false;
    }
    if (!sendTransport->isReady() || !recvTransport->isReady()) {
      std::cerr << "peel_allreduce_ring: transport not ready for rank "
                << rank_ << "\n";
      return false;
    }

    std::memcpy(outbox_, ptrs_[0], bytes_);

    const int numRounds = worldSize - 1;
    for (int round = 0; round < numRounds; ++round) {
      recvTransport->submitWork(hops_[recvHopIdx].sender, inbox_, bytes_);
      sendTransport->submitWork(hops_[sendHopIdx].sender, outbox_, bytes_);

      if (!recvTransport->waitResult()) {
        std::cerr << "peel_allreduce_ring: recv hop failed in round " << round
                  << ": " << hops_[recvHopIdx].sender << " -> "
                  << hops_[recvHopIdx].receiver << "\n";
        return false;
      }

      fn_->call(ptrs_[0], inbox_, count_);

      if (!sendTransport->waitResult()) {
        std::cerr << "peel_allreduce_ring: send hop failed in round " << round
                  << ": " << hops_[sendHopIdx].sender << " -> "
                  << hops_[sendHopIdx].receiver << "\n";
        return false;
      }

      if (round < (numRounds - 1)) {
        std::memcpy(outbox_, inbox_, bytes_);
      }
    }

    for (size_t i = 1; i < ptrs_.size(); ++i) {
      std::memcpy(ptrs_[i], ptrs_[0], bytes_);
    }

    return true;
  }

 private:
  int rank_;
  std::vector<PeelRingHop> hops_;
  std::vector<T*> ptrs_;
  const int count_;
  const size_t bytes_;
  const ReductionFunction<T>* fn_;

  T* inbox_{nullptr};
  T* outbox_{nullptr};
};

} // namespace peel
} // namespace transport
} // namespace gloo
