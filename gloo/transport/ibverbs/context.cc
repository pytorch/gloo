/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/ibverbs/context.h"

#include "gloo/common/error.h"
#include "gloo/transport/ibverbs/device.h"
#include "gloo/transport/ibverbs/pair.h"
#include "gloo/transport/ibverbs/remote_key.h"
#include "gloo/transport/ibverbs/unbound_buffer.h"

namespace gloo {
namespace transport {
namespace ibverbs {

Context::Context(std::shared_ptr<Device> device, int rank, int size)
    : ::gloo::transport::Context(rank, size), device_(device) {}

Context::~Context() {}

std::unique_ptr<transport::Pair>& Context::createPair(int rank) {
  pairs_[rank] = std::unique_ptr<transport::Pair>(
      new ibverbs::Pair(rank, device_, getTimeout()));
  return pairs_[rank];
}

std::unique_ptr<transport::UnboundBuffer> Context::createUnboundBuffer(
    void* ptr,
    size_t size) {
  return std::make_unique<UnboundBuffer>(this->shared_from_this(), ptr, size);
}

void Context::signalException(const std::string& msg) {
  // The `pairs_` vector is logically constant. After the context and
  // all of its pairs have been created it is not mutated until the
  // context is destructed. Therefore, we don't need to acquire this
  // context's instance lock before looping over `pairs_`.
  for (auto& pair : pairs_) {
    if (pair) {
      reinterpret_cast<ibverbs::Pair*>(pair.get())->signalIoFailure(msg);
    }
  }
}

std::unique_ptr<::gloo::transport::RemoteKey> Context::deserializeRemoteKey(
    const std::string& serialized) {
  return RemoteKey::deserialize(serialized);
}

} // namespace ibverbs
} // namespace transport
} // namespace gloo
