/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstring>
#include <vector>

#include "gloo/algorithm.h"
#include "gloo/common/common.h"
#include "gloo/common/logging.h"

namespace gloo {

template <typename T>
class BroadcastOneToAll : public Algorithm {
 public:
  BroadcastOneToAll(
      const std::shared_ptr<Context>& context,
      const std::vector<T*>& ptrs,
      size_t count,
      int rootRank = 0,
      int rootPointerRank = 0)
      : Algorithm(context),
        ptrs_(ptrs),
        count_(count),
        bytes_(count * sizeof(T)),
        rootRank_(rootRank),
        rootPointerRank_(rootPointerRank) {
    GLOO_ENFORCE_GE(rootRank_, 0);
    GLOO_ENFORCE_LT(rootRank_, contextSize_);
    GLOO_ENFORCE_GE(rootPointerRank_, 0);
    GLOO_ENFORCE_LT(rootPointerRank_, ptrs_.size());
  }

  void run() override {
    if (contextSize_ == 1) {
      broadcastLocally();
      return;
    }

    auto clearToSendBuffer = context_->createUnboundBuffer(nullptr, 0);
    auto buffer =
        context_->createUnboundBuffer(ptrs_[rootPointerRank_], bytes_);
    auto slot = context_->nextSlot();
    auto timeout = context_->getTimeout();

    if (contextRank_ == rootRank_) {
      // Fire off send operations after receiving clear to send
      for (auto i = 0; i < contextSize_; i++) {
        if (i == contextRank_) {
          continue;
        }
        clearToSendBuffer->recv(i, slot);
        clearToSendBuffer->waitRecv(timeout);
        buffer->send(i, slot);
      }

      // Broadcast locally while sends are happening
      broadcastLocally();

      // Wait for all send operations to complete
      for (auto i = 0; i < contextSize_; i++) {
        if (i == contextRank_) {
          continue;
        }
        buffer->waitSend(timeout);
      }
    } else {
      clearToSendBuffer->send(rootRank_, slot);
      clearToSendBuffer->waitSend(timeout);
      buffer->recv(rootRank_, slot);
      buffer->waitRecv(timeout);

      // Broadcast locally after receiving from root
      broadcastLocally();
    }
  }

 protected:
  // Broadcast from root pointer to other pointers
  void broadcastLocally() {
    for (auto i = 0; i < ptrs_.size(); i++) {
      if (i == rootPointerRank_) {
        continue;
      }

      memcpy(ptrs_[i], ptrs_[rootPointerRank_], bytes_);
    }
  }

  std::vector<T*> ptrs_;
  const size_t count_;
  const size_t bytes_;
  const int rootRank_;
  const int rootPointerRank_;
};

} // namespace gloo
