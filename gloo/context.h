/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include <gloo/transport/context.h>
#include <gloo/transport/pair.h>

namespace gloo {

// There is no need to materialize all transport types here.
namespace transport {
class Context;
class Device;
class UnboundBuffer;
} // namespace transport

class Context {
 public:
  Context(int rank, int size, int base = 2);
  ~Context();

  const int rank;
  const int size;
  int base;

  std::shared_ptr<transport::Device>& getDevice();

  std::unique_ptr<transport::Pair>& getPair(int i);

  // Factory function to create an unbound buffer for use with the
  // transport used for this context. Use this function to avoid tying
  // downstream code to a specific transport.
  std::unique_ptr<transport::UnboundBuffer> createUnboundBuffer(
      void* ptr,
      size_t size);

  int nextSlot(int numToSkip = 1);

  void closeConnections();

  void setTimeout(std::chrono::milliseconds timeout);

  std::chrono::milliseconds getTimeout() const;

  std::unique_ptr<transport::RemoteKey> deserializeRemoteKey(
      const std::string& serialized) {
    return transportContext_->deserializeRemoteKey(serialized);
  }

 protected:
  std::shared_ptr<transport::Device> device_;
  std::shared_ptr<transport::Context> transportContext_;
  int slot_;
  std::chrono::milliseconds timeout_;
};

} // namespace gloo
