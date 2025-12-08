/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/barrier.h"

namespace gloo {

class BarrierAllToAll : public Barrier {
 public:
  explicit BarrierAllToAll(const std::shared_ptr<Context>& context)
      : Barrier(context) {}

  void run() override {
    // Create send/recv buffers for every peer
    auto slot = this->context_->nextSlot();

    auto buffer = this->context_->createUnboundBuffer(nullptr, 0);
    auto timeout = this->context_->getTimeout();

    for (auto i = 0; i < this->contextSize_; i++) {
      // Skip self
      if (i == this->contextRank_) {
        continue;
      }
      buffer->send(i, slot);
      buffer->recv(i, slot);
    }

    for (auto i = 0; i < this->contextSize_; i++) {
      // Skip self
      if (i == this->contextRank_) {
        continue;
      }
      buffer->waitSend(timeout);
      buffer->waitRecv(timeout);
    }
  }
};

} // namespace gloo
