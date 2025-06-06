/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/transport/context.h"

#include <memory>

namespace gloo {
namespace transport {
namespace ibverbs {

// Forward declaration
class Device;
class Pair;

class Context : public ::gloo::transport::Context,
                public std::enable_shared_from_this<Context> {
 public:
  Context(std::shared_ptr<Device> device, int rank, int size);

  virtual ~Context();

  std::unique_ptr<transport::Pair>& createPair(int rank) override;

  std::unique_ptr<transport::UnboundBuffer> createUnboundBuffer(
      void* ptr,
      size_t size) override;

  // Set exception on every pair in this context. This is called when
  // waiting for a send or recv operation on an unbound buffer times
  // out. All pairs should be signaled and closed in that event.
  void signalException(const std::string& msg);

  std::unique_ptr<::gloo::transport::RemoteKey> deserializeRemoteKey(
      const std::string& serialized) override;

 protected:
  std::shared_ptr<Device> device_;

  friend class Pair;
  friend class UnboundBuffer;
};

} // namespace ibverbs
} // namespace transport
} // namespace gloo
