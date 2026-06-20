#include "gloo/peel_broadcast_ring.h"

#include <stdexcept>

namespace gloo {

void peel_broadcast_ring(PeelBroadcastRingOptions& opts) {
  if (!opts.peelContext) {
    throw std::runtime_error("peel_broadcast_ring: peelContext is null");
  }
  if (!opts.peelContext->isReady()) {
    throw std::runtime_error("peel_broadcast_ring: PeelContext is not ready");
  }
  if (!opts.ptr || opts.size == 0) {
    throw std::runtime_error("peel_broadcast_ring: invalid buffer");
  }
  if (!opts.peelContext->broadcastRing(opts.root, opts.ptr, opts.size)) {
    throw std::runtime_error("peel_broadcast_ring: broadcast failed");
  }
}

void peel_broadcast_ring(
    transport::peel::PeelContext* peelContext,
    int root,
    void* data,
    size_t size) {
  PeelBroadcastRingOptions opts;
  opts.peelContext = peelContext;
  opts.root = root;
  opts.ptr = data;
  opts.size = size;
  peel_broadcast_ring(opts);
}

} // namespace gloo
