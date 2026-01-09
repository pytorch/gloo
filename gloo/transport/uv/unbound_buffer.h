/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gloo/transport/unbound_buffer.h>

#include <condition_variable>
#include <memory>
#include <mutex>

#include <gloo/common/memory.h>

namespace gloo {
namespace transport {
namespace uv {

// Forward declaration
class Context;
class Pair;

class UnboundBuffer : public ::gloo::transport::UnboundBuffer {
 public:
  UnboundBuffer(std::shared_ptr<Context> context, void* ptr, size_t size);

  ~UnboundBuffer() override;

  // If specified, the source of this recv is stored in the rank pointer.
  // Returns true if it completed, false if it was aborted.
  bool waitRecv(int* rank, std::chrono::milliseconds timeout) override;

  // If specified, the destination of this send is stored in the rank pointer.
  // Returns true if it completed, false if it was aborted.
  bool waitSend(int* rank, std::chrono::milliseconds timeout) override;

  // Aborts a pending waitRecv call.
  void abortWaitRecv() override;

  // Aborts a pending waitSend call.
  void abortWaitSend() override;

  void send(int dstRank, uint64_t tag, size_t offset, size_t nbytes = 0)
      override;

  void recv(int srcRank, uint64_t tag, size_t offset, size_t nbytes = 0)
      override;

  void recv(
      std::vector<int> srcRanks,
      uint64_t slot,
      size_t offset,
      size_t nbytes) override;

 protected:
  void handleRecvCompletion(int rank);
  void handleSendCompletion(int rank);

  std::shared_ptr<Context> context_;

  std::mutex mutex_;
  std::condition_variable recvCv_;
  std::condition_variable sendCv_;
  bool abortWaitRecv_{false};
  bool abortWaitSend_{false};

  int recvCompletions_{0};
  int recvRank_{-1};
  int sendCompletions_{0};
  int sendRank_{-1};

  // Allows for sharing weak (non owning) references to "this" without
  // affecting the lifetime of this instance.
  ShareableNonOwningPtr<UnboundBuffer> shareableNonOwningPtr_;

  // Returns weak reference to "this". See pair.{h,cc} for usage.
  inline WeakNonOwningPtr<UnboundBuffer> getWeakNonOwningPtr() const {
    return WeakNonOwningPtr<UnboundBuffer>(shareableNonOwningPtr_);
  }

  friend class Context;
  friend class Pair;
};

} // namespace uv
} // namespace transport
} // namespace gloo
