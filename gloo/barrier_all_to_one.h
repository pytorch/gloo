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

class BarrierAllToOne : public Barrier {
 public:
  explicit BarrierAllToOne(
      const std::shared_ptr<Context>& context,
      int rootRank = 0)
      : Barrier(context), rootRank_(rootRank) {}

  void run() {
    auto slot = this->context_->nextSlot();
    auto timeout = this->context_->getTimeout();

    auto buffer = this->context_->createUnboundBuffer(nullptr, 0);

    if (this->contextRank_ == rootRank_) {
      for (int i = 0; i < this->contextSize_; i++) {
        // Skip self
        if (i == this->contextRank_) {
          continue;
        }
        buffer->recv(i, slot);
        buffer->waitRecv(timeout);
      }
      for (int i = 0; i < this->contextSize_; i++) {
        // Skip self
        if (i == this->contextRank_) {
          continue;
        }
        buffer->send(i, slot);
        buffer->waitSend(timeout);
      }

    } else {
      buffer->send(rootRank_, slot);
      buffer->waitSend(timeout);
      buffer->recv(rootRank_, slot);
      buffer->waitRecv(timeout);
    }
  }

 protected:
  const int rootRank_;
};

} // namespace gloo
