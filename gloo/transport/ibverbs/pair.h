/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gloo/transport/ibverbs/address.h"
#include "gloo/transport/ibverbs/device.h"
#include "gloo/transport/ibverbs/memory_region.h"
#include "gloo/transport/pair.h"

namespace gloo {
namespace transport {
namespace ibverbs {

// Forward declaration
class Buffer;

class BufferHandler {
 public:
  virtual ~BufferHandler() = default;

  virtual void handleCompletion(int rank, struct ibv_wc* wc) = 0;
  virtual void signalError(const std::exception_ptr& ex) = 0;

  virtual bool isPeristentHandler() {
    return false;
  }
};

class Pair : public ::gloo::transport::Pair {
  static constexpr int kMaxBuffers = 8;
  static constexpr auto kRecvCompletionQueueCapacity = kMaxBuffers;
  static constexpr auto kSendCompletionQueueCapacity = kMaxBuffers;
  static constexpr auto kCompletionQueueCapacity =
      kRecvCompletionQueueCapacity + kSendCompletionQueueCapacity;

  // The ibv_req_notify(3) function takes an argument called
  // 'solicited_only' which makes it only trigger a notification for
  // work requests that are flagged as solicited. Every completion
  // should trigger a notification, so always pass 0.
  static constexpr auto kNotifyOnAnyCompletion = 0;

 public:
  explicit Pair(
      int rank,
      const std::shared_ptr<Device>& dev,
      std::chrono::milliseconds timeout);

  virtual ~Pair();

  Pair(const Pair& that) = delete;

  Pair& operator=(const Pair& that) = delete;

  virtual const Address& address() const override;

  virtual void connect(const std::vector<char>& bytes) override;

  virtual void setSync(bool enable, bool busyPoll) override;

  virtual std::unique_ptr<::gloo::transport::Buffer> createSendBuffer(
      int slot,
      void* ptr,
      size_t size) override;

  virtual std::unique_ptr<::gloo::transport::Buffer> createRecvBuffer(
      int slot,
      void* ptr,
      size_t size) override;

  virtual bool isConnected() override;

  // Send from the specified buffer to remote side of pair.
  virtual void send(
      transport::UnboundBuffer* tbuf,
      uint64_t tag,
      size_t offset,
      size_t nbytes) override;

  // Receive into the specified buffer from the remote side of pair.
  virtual void recv(
      transport::UnboundBuffer* tbuf,
      uint64_t tag,
      size_t offset,
      size_t nbytes) override;

  void put(
      transport::UnboundBuffer* tbuf,
      const transport::RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) override;

  void get(
      transport::UnboundBuffer* tbuf,
      const transport::RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) override;

  void handleCompletionEvent();

  void pollCompletions();

  void handleCompletion(struct ibv_wc* wc);

  void send(Buffer* buf, size_t offset, size_t length, size_t roffset);

  void close() override;

  void signalIoFailure(const std::string& msg);

 protected:
  const int rank_;

  std::shared_ptr<Device> dev_;

  // Whether or not this pair is running in sync mode.
  std::atomic<bool> sync_;

  // Whether or not this pair is busy polling in sync mode.
  std::atomic<bool> busyPoll_;

  const std::chrono::milliseconds timeout_;

  // Number of completion events handled by this pair's completion
  // queue (also see ibv_get_cq_event(3)). This many events need to be
  // acknowledged prior to destructing the completion queue.
  // Otherwise, destruction will hang (see ibv_get_cq_event(3)).
  int completionEventsHandled_;

  Address self_;
  Address peer_;

  struct ibv_cq* cq_;
  struct ibv_qp* qp_;

  std::mutex m_;
  std::condition_variable cv_;

  // For us to copy the remote peer's ibv_mr into.
  std::map<int, std::deque<struct ibv_mr>> peerMemoryRegions_;
  std::map<int, std::deque<std::function<void(struct ibv_mr)>>>
      recvMemoryRegionCallbacks_;

  // These fields store memory regions that the remote side of the pair
  // can send to and that the local side of the pair can send from.
  //
  // When registering a receive buffer, the local ibv_mr is sent
  // to the remote side of the pair, and the corresponding MemoryRegion
  // instance is kept around in the mappedSendRegions_ list until
  // the send operation complete.
  //
  // To allow the remote side of the pair to send its memory regions,
  // we keep a fixed number of MemoryRegion instances in
  // mappedRecvRegions_. These regions are referenced round-robin for
  // every posted receive work request.
  //
  std::map<int, std::deque<std::unique_ptr<MemoryRegion>>> mappedSendRegions_;
  std::array<std::unique_ptr<MemoryRegion>, kMaxBuffers> mappedRecvRegions_;

  // Keep track of number of request work requests posted and completed.
  // This is needed to index into the mappedRecvRegions_ array both
  // when posting the WR and when completing the WR.
  uint64_t recvPosted_;

  // Completions on behalf of buffers need to be forwarded to those buffers.
  std::map<int, std::deque<BufferHandler*>> sendCompletionHandlers_;
  std::map<int, std::deque<BufferHandler*>> recvCompletionHandlers_;

  void sendMemoryRegion(struct ibv_mr* mr, int slot);
  void recvMemoryRegion(
      std::unique_lock<std::mutex>& lock,
      int slot,
      std::function<void(struct ibv_mr)> callback);

  void postReceive();

  std::chrono::milliseconds getTimeout() const {
    return timeout_;
  }

  const Address& peer() const {
    return peer_;
  }

 private:
  std::exception_ptr ex_;
  bool closed_ = false;

  // Used to signal IO exceptions from one thread and propagate onto others.
  void checkErrorState();

  friend class Buffer;
  friend class UnboundBuffer;
};

} // namespace ibverbs
} // namespace transport
} // namespace gloo
