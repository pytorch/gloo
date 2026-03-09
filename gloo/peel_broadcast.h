// gloo/peel_broadcast.h

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// Only depend on TCP transport
#include "gloo/transport/tcp/context.h"
#include "gloo/transport/tcp/peel/peel_context.h"

namespace gloo {

/**
 * Peel broadcast options
 * Note: This ONLY works with TCP transport
 */
struct PeelBroadcastOptions {
  // TCP context with Peel enabled (NOT gloo::Context)
  transport::tcp::Context* tcpContext = nullptr;

  // Root rank (sender)
  int root = 0;

  // Buffer
  void* ptr = nullptr;
  size_t size = 0;

  // Helpers
  template <typename T>
  void setOutput(T* p, size_t count) {
    ptr = static_cast<void*>(p);
    size = count * sizeof(T);
  }

  template <typename T>
  void setOutput(std::vector<T>& v) {
    setOutput(v.data(), v.size());
  }
};

/**
 * Check if Peel is available on the given TCP context
 */
bool isPeelAvailable(const transport::tcp::Context* tcpContext);

/**
 * Perform broadcast using Peel multicast
 * 
 * @param opts Options with TCP context
 * @throws std::runtime_error on failure
 */
void peel_broadcast(PeelBroadcastOptions& opts);

/**
 * Convenience overload
 */
void peel_broadcast(
    transport::tcp::Context* tcpContext,
    int root,
    void* data,
    size_t size);

/**
 * Template version
 */
template <typename T>
void peel_broadcast(
    transport::tcp::Context* tcpContext,
    int root,
    std::vector<T>& data) {
  peel_broadcast(tcpContext, root, data.data(), data.size() * sizeof(T));
}

} // namespace gloo