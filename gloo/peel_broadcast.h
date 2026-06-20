// gloo/peel_broadcast.h
// Compatibility convenience wrapper for the standalone Peel transport.

#pragma once

#include <cstddef>
#include <vector>

#include "gloo/transport/peel/peel_context.h"

namespace gloo {

struct PeelBroadcastOptions {
  transport::peel::PeelContext* peelContext = nullptr;
  int root = 0;
  void* ptr = nullptr;
  size_t size = 0;

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

bool isPeelAvailable(const transport::peel::PeelContext* peelContext);
void peel_broadcast(PeelBroadcastOptions& opts);
void peel_broadcast(
    transport::peel::PeelContext* peelContext,
    int root,
    void* data,
    size_t size);

template <typename T>
void peel_broadcast(
    transport::peel::PeelContext* peelContext,
    int root,
    std::vector<T>& data) {
  peel_broadcast(peelContext, root, data.data(), data.size() * sizeof(T));
}

} // namespace gloo
