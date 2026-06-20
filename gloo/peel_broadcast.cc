#include "gloo/peel_broadcast.h"

#include <stdexcept>

namespace gloo {

bool isPeelAvailable(const transport::peel::PeelContext* peelContext) {
  return peelContext && peelContext->isReady();
}

void peel_broadcast(PeelBroadcastOptions& opts) {
  if (!opts.peelContext) {
    throw std::runtime_error("peel_broadcast: peelContext is null");
  }
  if (!opts.peelContext->isReady()) {
    throw std::runtime_error("peel_broadcast: PeelContext is not ready");
  }
  if (!opts.ptr || opts.size == 0) {
    throw std::runtime_error("peel_broadcast: invalid buffer");
  }
  if (!opts.peelContext->broadcast(opts.root, opts.ptr, opts.size)) {
    throw std::runtime_error("peel_broadcast: broadcast failed");
  }
}

void peel_broadcast(
    transport::peel::PeelContext* peelContext,
    int root,
    void* data,
    size_t size) {
  PeelBroadcastOptions opts;
  opts.peelContext = peelContext;
  opts.root = root;
  opts.ptr = data;
  opts.size = size;
  peel_broadcast(opts);
}

} // namespace gloo
