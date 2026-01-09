/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#include <gloo/common/memory.h>
#include <gloo/transport/pair.h>
#include <gloo/transport/uv/address.h>
#include <gloo/transport/uv/device.h>

namespace gloo {
namespace transport {
namespace uv {

// Forward declaration
class Context;

// Forward declaration
class UnboundBuffer;

struct Op {
  enum Opcode {
    SEND_UNBOUND_BUFFER = 1,
    NOTIFY_SEND_READY = 2,
    NOTIFY_RECV_READY = 3,
  };

  inline enum Opcode getOpcode() const {
    return static_cast<Opcode>(preamble.opcode);
  }

  struct {
    // Total number of bytes in this operation.
    size_t nbytes = 0;

    // Identifies the nature of this message.
    size_t opcode = 0;

    // Identifies the tag of this message.
    // This is used to match send/recv operations between both sides of a pair.
    size_t tag = 0;

    // Number of bytes to read/write.
    size_t length = 0;

  } preamble;

  // Number of bytes read/written.
  // Used to determine when we're done reading/writing an op.
  size_t nread = 0;
  size_t nwritten = 0;

  // For sending: pass buffer to write routine.
  // For receiving: store pointer and progress during read.
  NonOwningPtr<UnboundBuffer> buf;
  size_t offset = 0;
  size_t length = 0;
};

class Pair : public ::gloo::transport::Pair {
  enum State {
    INITIALIZED,
    CONNECTING,
    CONNECTED,
    CLOSING,
    CLOSED,
  };

 public:
  explicit Pair(
      Context* context,
      Device* device,
      int rank,
      std::chrono::milliseconds timeout);

  ~Pair() override;

  Pair(const Pair& that) = delete;

  Pair& operator=(const Pair& that) = delete;

  const Address& address() const override;

  void connect(const std::vector<char>& bytes) override;

  void setSync(bool sync, bool busyPoll) override {
    abort();
  }

  std::unique_ptr<::gloo::transport::Buffer> createSendBuffer(
      int slot,
      void* ptr,
      size_t size) override {
    abort();
  }

  std::unique_ptr<::gloo::transport::Buffer> createRecvBuffer(
      int slot,
      void* ptr,
      size_t size) override {
    abort();
  }

  // Send from the specified buffer to remote side of pair.
  void send(
      transport::UnboundBuffer* tbuf,
      uint64_t tag,
      size_t offset,
      size_t nbytes) override;

  // Receive into the specified buffer from the remote side of pair.
  void recv(
      transport::UnboundBuffer* tbuf,
      uint64_t tag,
      size_t offset,
      size_t nbytes) override;

  // Attempt to receive into the specified buffer from the remote side
  // of pair. Returns true if there was a remote pending send and the
  // recv is in progress, false otherwise.
  bool tryRecv(
      transport::UnboundBuffer* tbuf,
      uint64_t tag,
      size_t offset,
      size_t nbytes);

  void close() override;

  bool isConnected() override;

 private:
  std::mutex mutex_;
  std::condition_variable cv_;

  // Details for pending send/recv operations.
  struct UnboundBufferOp {
    UnboundBufferOp(
        WeakNonOwningPtr<UnboundBuffer> buf,
        size_t offset,
        size_t length)
        : buf(std::move(buf)), offset(offset), length(length) {}

    WeakNonOwningPtr<UnboundBuffer> buf;
    size_t offset = 0;
    size_t length = 0;
  };

  // Keep track of pending send/recv operations by tag.
  std::unordered_map<uint64_t, std::deque<UnboundBufferOp>> localPendingSend_;
  std::unordered_map<uint64_t, std::deque<UnboundBufferOp>> localPendingRecv_;

  // Refer to parent context using raw pointer. This could be a
  // weak_ptr, seeing as the context class is a shared_ptr, but:
  // 1) That means calling std::weak_ptr::lock() everytime we need it,
  // 2) The context holds a unique_ptr to this pair, so the context
  //    pointer will be valid for the lifetime of this pair.
  Context* const context_;

  // Refer to device using raw pointer. The context owns a shared_ptr
  // to the device, and per the lifetime guarantees of the context,
  // there is no need to duplicate that shared_ptr in this class.
  Device* const device_;

  // Rank of the process this pair connects to.
  const int rank_;

  // Timeout for operations executed against this pair.
  const std::chrono::milliseconds timeout_;

  // The address for this pair is represented by the sockaddr of the
  // listening socket (owned by the device instance) and a unique
  // identifier. The address is shared with its peer through some
  // external mechanism (see the `./gloo/rendezvous` directory).
  Address addr_;

  // State of the pair. This is used so that we can ensure the
  // underlying connection is closed before we destruct.
  State state_{INITIALIZED};

  // Error state of the handle, if set.
  int errno_{0};

  // Handle of the connection.
  // This is set only if state_ == CONNECTED || state_ == CLOSING.
  std::shared_ptr<libuv::TCP> handle_;

  // Pending read operation.
  // Its state needs to be kept around in case it takes multiple
  // read(2) calls to complete.
  Op readOp_;

  // List of pending write operations.
  // They are kept around because writes complete asynchronously.
  std::deque<Op> writeOps_;

  // This function is called from the device thread when this pair's
  // connection has been established or an error occurred.
  void connectCallback(std::shared_ptr<libuv::TCP>, const libuv::ErrorEvent&);

  // Instructs handler to read operation from peer.
  void readNextOp();

  // Called if the handle is closed and can be destroyed.
  void onClose(const libuv::CloseEvent&, const libuv::TCP&);

  // Called if the handle received an EOF from its peer.
  void onEnd(const libuv::EndEvent&, const libuv::TCP&);

  // Called if the handle saw an error.
  void onError(const libuv::ErrorEvent&, const libuv::TCP&);

  // Called on read completion.
  void onRead(const libuv::ReadEvent&, const libuv::TCP&);

  // Called on write completion.
  void onWrite(const libuv::WriteEvent&, const libuv::TCP&);

  // Called on receiving a SEND_UNBOUND_BUFFER operation.
  void onSendUnboundBuffer(const Op& op);

  // Called on receiving a NOTIFY_SEND_READY operation.
  void onNotifySendReady(const Op& op);

  // Called on receiving a NOTIFY_RECV_READY operation.
  void onNotifyRecvReady(const Op& op);

  // Perform asynchronous socket write(s) for operation.
  void writeOp(Op op);

  // Send notification to peer that there is a pending send operation.
  void sendNotifySendReady(uint64_t tag, size_t nbytes);

  // Send notification to peer that there is a pending recv operation.
  void sendNotifyRecvReady(uint64_t tag, size_t nbytes);

  // Send unbound buffer to peer.
  void sendUnboundBuffer(
      uint64_t tag,
      NonOwningPtr<UnboundBuffer> buf,
      size_t offset,
      size_t length);

  // Closes handle_, if applicable.
  // Assumes the caller holds the instance lock.
  void closeWhileHoldingPairLock();
};

} // namespace uv
} // namespace transport
} // namespace gloo
