/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>

#include <infiniband/verbs.h>

#include "gloo/transport/buffer.h"
#include "gloo/transport/ibverbs/device.h"
#include "gloo/transport/ibverbs/pair.h"

namespace gloo {
namespace transport {
namespace ibverbs {

class Buffer : public ::gloo::transport::Buffer, public BufferHandler {
 public:
  virtual ~Buffer();

  virtual void send(size_t offset, size_t length, size_t roffset = 0) override;

  virtual void waitRecv() override;
  virtual void waitSend() override;

  void handleCompletion(int rank, struct ibv_wc* wc) override;

  void signalError(const std::exception_ptr& ex) override;
  void checkErrorState();

  bool isPeristentHandler() override {
    return true;
  }

 protected:
  // May only be constructed from helper function in pair.cc
  Buffer(Pair* pair, int slot, void* ptr, size_t size);

  Pair* pair_;

  // Empty buffer to use when a nullptr buffer is created.
  char emptyBuf_[1];

  struct ibv_mr* mr_;
  std::unique_ptr<struct ibv_mr> peerMr_;

  std::mutex m_;
  std::condition_variable recvCv_;
  std::condition_variable sendCv_;

  int recvCompletions_;
  int sendCompletions_;
  std::atomic<int> sendPending_;

  std::exception_ptr ex_;

  friend class Pair;
};

} // namespace ibverbs
} // namespace transport
} // namespace gloo
