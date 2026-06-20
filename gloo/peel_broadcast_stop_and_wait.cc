#include "gloo/peel_broadcast_stop_and_wait.h"

#include <stdexcept>

namespace gloo {

void peel_broadcast_stop_and_wait(PeelBroadcastStopAndWaitOptions& opts) {
  if (!opts.peelContext) {
    throw std::runtime_error("peel_broadcast_stop_and_wait: peelContext is null");
  }
  if (!opts.peelContext->isReady()) {
    throw std::runtime_error(
        "peel_broadcast_stop_and_wait: PeelContext is not ready");
  }
  if (!opts.ptr || opts.size == 0) {
    throw std::runtime_error("peel_broadcast_stop_and_wait: invalid buffer");
  }
  if (!opts.peelContext->broadcastStopAndWait(opts.root, opts.ptr, opts.size)) {
    throw std::runtime_error("peel_broadcast_stop_and_wait: broadcast failed");
  }
}

void peel_broadcast_stop_and_wait(
    transport::peel::PeelContext* peelContext,
    int root,
    void* data,
    size_t size) {
  PeelBroadcastStopAndWaitOptions opts;
  opts.peelContext = peelContext;
  opts.root = root;
  opts.ptr = data;
  opts.size = size;
  peel_broadcast_stop_and_wait(opts);
}

} // namespace gloo
