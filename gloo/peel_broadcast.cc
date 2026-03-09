#include "gloo/peel_broadcast.h"

#include <stdexcept>

namespace gloo {

bool isPeelAvailable(const transport::tcp::Context* tcpContext) {
  return tcpContext && tcpContext->isPeelReady();
}

void peel_broadcast(PeelBroadcastOptions& opts) {
  if (!opts.tcpContext) {
    throw std::runtime_error("peel_broadcast: tcpContext is null");
  }

  if (!opts.tcpContext->isPeelReady()) {
    throw std::runtime_error(
        "peel_broadcast: Peel not initialized. Call enablePeel() first.");
  }

  if (!opts.ptr || opts.size == 0) {
    throw std::runtime_error("peel_broadcast: invalid buffer");
  }

  if (opts.root < 0 || opts.root >= opts.tcpContext->size) {
    throw std::runtime_error("peel_broadcast: invalid root rank");
  }

  bool success = opts.tcpContext->peelBroadcast(opts.root, opts.ptr, opts.size);
  if (!success) {
    throw std::runtime_error("peel_broadcast: broadcast failed");
  }
}

void peel_broadcast(
    transport::tcp::Context* tcpContext,
    int root,
    void* data,
    size_t size) {
  
  PeelBroadcastOptions opts;
  opts.tcpContext = tcpContext;
  opts.root = root;
  opts.ptr = data;
  opts.size = size;

  peel_broadcast(opts);
}

} // namespace gloo