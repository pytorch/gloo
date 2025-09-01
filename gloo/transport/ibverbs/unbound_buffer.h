/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/common/memory.h"
#include "gloo/transport/ibverbs/device.h"
#include "gloo/transport/ibverbs/pair.h"
#include "gloo/transport/ibverbs/remote_key.h"
#include "gloo/transport/unbound_buffer.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace gloo {
namespace transport {
namespace ibverbs {

// Forward declaration
class Context;
class Pair;

class UnboundBuffer : public ::gloo::transport::UnboundBuffer,
                      public BufferHandler {
 public:
  UnboundBuffer(
      const std::shared_ptr<Context>& context,
      void* ptr,
      size_t size);

  virtual ~UnboundBuffer();

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

  void send(int dstRank, uint64_t slot, size_t offset, size_t nbytes) override;

  void recv(int srcRank, uint64_t slot, size_t offset, size_t nbytes) override;

  void recv(
      std::vector<int> srcRanks,
      uint64_t slot,
      size_t offset,
      size_t nbytes) override;

  void handleCompletion(int rank, struct ibv_wc* wc);
  // Set exception and wake up any waitRecv/waitSend threads.
  void signalError(const std::exception_ptr&) override;

  virtual std::unique_ptr<::gloo::transport::RemoteKey> getRemoteKey()
      const override;

  virtual void put(
      const transport::RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) override;

  virtual void get(
      const transport::RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) override;

 protected:
  std::shared_ptr<Context> context_;

  // Empty buffer to use when a nullptr buffer is created.
  char emptyBuf_[1];

  std::mutex m_;
  std::condition_variable recvCv_;
  std::condition_variable sendCv_;
  bool abortWaitRecv_{false};
  bool abortWaitSend_{false};

  struct ibv_mr* mr_;
  std::optional<RemoteKey> remoteKey_;

  std::deque<int> recvCompletions_;
  int recvRank_;
  int sendCompletions_;
  int sendRank_;
  int sendPending_{0};

  std::exception_ptr ex_;

  // Throws if an exception if set.
  void throwIfException();

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

} // namespace ibverbs
} // namespace transport
} // namespace gloo
