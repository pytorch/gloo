#pragma once

#include "peel_broadcast_ring.h"

#include <cstddef>
#include <vector>

namespace gloo {
namespace transport {
namespace peel {

// Ring allgather on top of the same 2-rank Peel hop transports used by
// PeelBroadcastRing.  For world size N there are N logical edges:
//   i -> (i + 1) % N
// and each process locally owns up to two transports:
//   left  edge: (rank - 1 + N) % N -> rank
//   right edge: rank -> (rank + 1) % N
//
// In round k, rank r sends the segment originating from:
//   sendSrc = (r - k + N) % N
// and receives the segment originating from:
//   recvSrc = (r - k - 1 + N) % N
// which matches the dataflow of Gloo's AllgatherRing.
class PeelAllgatherRing {
 public:
  PeelAllgatherRing(int rank, std::vector<PeelRingHop> hops);

  // bufs[src] points to the storage for the segment produced by rank=src.
  // The local rank must already have initialized bufs[rank_].  After run()
  // returns successfully, every bufs[src] contains that rank's payload.
  bool run(const std::vector<void*>& bufs, size_t size);

 private:
  int rank_;
  std::vector<PeelRingHop> hops_;
};

} // namespace peel
} // namespace transport
} // namespace gloo
