/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/uv/pair.h>

#include <cstring>
#include <iostream>

#include <gloo/common/error.h>
#include <gloo/common/logging.h>
#include <gloo/transport/uv/common.h>
#include <gloo/transport/uv/context.h>
#include <gloo/transport/uv/libuv.h>
#include <gloo/transport/uv/unbound_buffer.h>

namespace gloo {
namespace transport {
namespace uv {

Pair::Pair(
    Context* context,
    Device* device,
    int rank,
    std::chrono::milliseconds timeout)
    : context_(context),
      device_(device),
      rank_(rank),
      timeout_(timeout),
      addr_(device_->nextAddress())
      {}

Pair::~Pair() {
  std::unique_lock<std::mutex> lock(mutex_);

  // The handle may still be around and must be closed.
  // To prevent event listeners for this handle to fire
  // after this destructor has returned, we must wait for
  // the call to close the handle to have executed.
  closeWhileHoldingPairLock();

  // Wait for call to close handle to execute.
  cv_.wait(lock, [&] { return state_ == CLOSED; });
}

const Address& Pair::address() const {
  return addr_;
}

bool Pair::isConnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == CONNECTED;
}

void Pair::connect(const std::vector<char>& bytes) {
  const auto peer = Address(bytes);

  std::unique_lock<std::mutex> lock(mutex_);
  GLOO_ENFORCE_EQ(state_, INITIALIZED);
  state_ = CONNECTING;

  // Both processes call the `Pair::connect` function with the address
  // of the other. The device instance associated with both `Pair`
  // instances is responsible for establishing the actual connection,
  // seeing as it owns the listening socket.
  //
  // One side takes a passive role and the other side takes an active
  // role in establishing the connection. The passive role means
  // waiting for an incoming connection that identifies itself with a
  // specific sequence number (encoded in the `Address`). The active
  // role means creating a connection to a specific address, and
  // writing out a specific sequence number. Once the process for
  // either role succeeds, the connection callback for the pair gets
  // called with the object representing the underlying connection.
  //
  device_->connect(
      addr_,
      peer,
      timeout_,
      [this](auto && PH1, auto && PH2) { connectCallback(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); });

  // Wait for callback to fire.
  //
  // NOTE(pietern): This can be split out to a separate function so
  // that we first initiate all connections and then wait on all of
  // them. This should make context initialization a bit faster. It
  // requires a change to the base class though, so let's so it after
  // this new transport has been merged.
  //
  cv_.wait(lock, [&] { return state_ == CONNECTED || state_ == CLOSED; });

  if (errno_) {
    throw ::gloo::IoException(GLOO_ERROR_MSG(
        "Error connecting to ",
        peer.str(),
        ": ",
        libuv::ErrorEvent(errno_).what()));
  }
}

void Pair::connectCallback(
    std::shared_ptr<libuv::TCP> handle,
    const libuv::ErrorEvent& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error) {
      errno_ = error.code();
      state_ = CLOSED;
      return;
    }

    handle_ = std::move(handle);
    state_ = CONNECTED;

    // Setup event listeners.
    handle_->on<libuv::CloseEvent>([this](auto && PH1, auto && PH2) { onClose(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); });
    handle_->on<libuv::EndEvent>([this](auto && PH1, auto && PH2) { onEnd(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); });
    handle_->on<libuv::ErrorEvent>([this](auto && PH1, auto && PH2) { onError(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); });
    handle_->on<libuv::ReadEvent>([this](auto && PH1, auto && PH2) { onRead(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); });
    handle_->on<libuv::WriteEvent>([this](auto && PH1, auto && PH2) { onWrite(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); });

    // Prepare to read next preamble.
    readNextOp();
  }

  // Wake up connect function.
  cv_.notify_one();
}

// Make next read operation read an operation's preamble struct.
//
// Threading: called from event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::readNextOp() {
  // Reset pending operation.
  readOp_ = Op();
  // Next read must fill the preamble struct.
  handle_->read((char*)&readOp_.preamble, sizeof(readOp_.preamble));
}

// Called if the handle is closed and can be destroyed.
//
// Threading: called from event loop thread.
// Locking requirements: none.
//
void Pair::onClose(const libuv::CloseEvent& event, const libuv::TCP&) {
  std::unique_lock<std::mutex> lock(mutex_);
  state_ = CLOSED;

  // Hold the lock while notifying waiting threads. If the thread
  // executing `Pair::~Pair` is woken up, then we'll trigger a data
  // race between notification and destruction of the condition
  // variable (as reported by tsan).
  cv_.notify_all();
}

// Called if the handle received an EOF from its peer.
//
// Threading: called from event loop thread.
// Locking requirements: none.
//
void Pair::onEnd(const libuv::EndEvent& event, const libuv::TCP&) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == CONNECTED) {
      closeWhileHoldingPairLock();
    }
  }
  cv_.notify_all();
}

// Called if the handle saw an error.
//
// Threading: called from event loop thread.
// Locking requirements: none.
//
void Pair::onError(const libuv::ErrorEvent& event, const libuv::TCP&) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    errno_ = event.code();
    if (state_ == CONNECTED) {
      closeWhileHoldingPairLock();
    }
  }
  cv_.notify_all();
}

// Called on read completion.
//
// Threading: called from event loop thread.
// Locking requirements: none.
//
void Pair::onRead(const libuv::ReadEvent& event, const libuv::TCP&) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& op = readOp_;

  // If this is the first read for the current operation,
  // assert that we read the entire preamble.
  if (op.nread == 0) {
    GLOO_ENFORCE(event.length == sizeof(op.preamble));
  }

  // Capture total number of bytes read for the current operation.
  op.nread += event.length;

  const auto opcode = op.getOpcode();
  if (opcode == Op::SEND_UNBOUND_BUFFER) {
    // Remote side is sending data; find pending recv operation
    // and read into the associated unbound buffer.
    if (!op.buf) {
      auto it = localPendingRecv_.find(op.preamble.tag);
      GLOO_ENFORCE(it != localPendingRecv_.end());

      // Ensure queue of pending recv operations is not empty.
      auto& queue = it->second;
      GLOO_ENFORCE(!queue.empty());

      // Move pending recv operation to stack.
      auto pendingRecv = std::move(queue.front());
      queue.pop_front();
      if (queue.empty()) {
        localPendingRecv_.erase(it);
      }

      // Lock pointer to unbound buffer.
      op.buf = NonOwningPtr<UnboundBuffer>(pendingRecv.buf);
      GLOO_ENFORCE(op.buf, "Cannot lock pointer to unbound buffer");
      op.offset = pendingRecv.offset;
      op.length = pendingRecv.length;

      // Read into unbound buffer, if the read is non-empty.
      if (op.length) {
        handle_->read((char*)op.buf->ptr + op.offset, op.length);
        return;
      }
    }

    GLOO_ENFORCE(op.nread == op.preamble.nbytes);
    onSendUnboundBuffer(op);
  } else if (opcode == Op::NOTIFY_SEND_READY) {
    GLOO_ENFORCE(op.nread == op.preamble.nbytes);
    onNotifySendReady(op);
  } else if (opcode == Op::NOTIFY_RECV_READY) {
    GLOO_ENFORCE(op.nread == op.preamble.nbytes);
    onNotifyRecvReady(op);
  } else {
    FAIL("Unexpected opcode: ", opcode);
  }

  // Reset read operation and issue read for the next preamble.
  readNextOp();
}

// Called on receiving a SEND_UNBOUND_BUFFER operation.
//
// Threading: called from event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::onSendUnboundBuffer(const Op& op) {
  op.buf->handleRecvCompletion(rank_);
}

// Called on receiving a NOTIFY_SEND_READY operation.
//
// Threading: called from event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::onNotifySendReady(const Op& op) {
  const auto& tag = op.preamble.tag;

  // Acquire context lock through mutator.
  Context::Mutator mutator(*context_, tag, rank_);

  // If a receive operation was posted without there already being a
  // corresponding send notification, we'll find a pending send
  // notification and don't need to handle this send notification.
  if (mutator.shiftExpectedSendNotification()) {
    return;
  }

  {
    // If we're ready to add it to the context wide pending operation
    // tally, first check if there are any recv-from-any operations
    // that this send operation can fulfill.
    WeakNonOwningPtr<UnboundBuffer> buf;
    size_t offset;
    size_t nbytes;
    if (context_->findRecvFromAny(tag, rank_, &buf, &offset, &nbytes)) {
      localPendingRecv_[tag].emplace_back(std::move(buf), offset, nbytes);
      sendNotifyRecvReady(tag, nbytes);
      return;
    }
  }

  // Increase balance of remote pending sends.
  mutator.pushRemotePendingSend();
}

// Called on receiving a NOTIFY_RECV_READY operation.
//
// Threading: called from event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::onNotifyRecvReady(const Op& op) {
  const auto& tag = op.preamble.tag;

  // Find local pending send and execute it.
  // Nothing to do if there are none.
  auto it = localPendingSend_.find(tag);
  if (it != localPendingSend_.end()) {
    auto& queue = it->second;
    GLOO_ENFORCE(!queue.empty());
    auto nextOp = std::move(queue.front());

    // Remove operation from queue, and potentially remove map entry
    // for this tag.
    queue.pop_front();
    if (queue.empty()) {
      localPendingSend_.erase(it);
    }

    auto buf = NonOwningPtr<UnboundBuffer>(nextOp.buf);
    GLOO_ENFORCE(buf, "Cannot lock pointer to unbound buffer");
    sendUnboundBuffer(tag, std::move(buf), nextOp.offset, nextOp.length);
    return;
  }

  // Increase balance of remote pending recv.
  // Note that the current value CANNOT be negative, as sends
  // cannot execute until the remote side is ready to receive.
  Context::Mutator mutator(*context_, tag, rank_);
  mutator.pushRemotePendingRecv();
}

// Called on write completion.
//
// Threading: called from event loop thread.
// Locking requirements: none.
//
void Pair::onWrite(const libuv::WriteEvent& event, const libuv::TCP&) {
  std::unique_lock<std::mutex> lock(mutex_);

  auto& ref = writeOps_.front();
  if (ref.getOpcode() == Op::SEND_UNBOUND_BUFFER) {
    // Ops of type SEND_UNBOUND_BUFFER are written with 2 calls.
    // If this is the first completion, record it and return.
    if (ref.nwritten == 0) {
      ref.nwritten += sizeof(ref.preamble);
      return;
    }
    // Let unbound buffer know this send operation has completed.
    GLOO_ENFORCE(ref.buf);
    ref.buf->handleSendCompletion(rank_);
  }

  // Every completion corresponds to one or two completion calls.
  writeOps_.pop_front();
}

// Perform asynchronous socket write(s) for operation.
//
// Threading: called from either user thread or event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::writeOp(Op op) {
  writeOps_.push_back(std::move(op));

  // No need to copy and make another heap allocation for this op.
  // It is already stored in writeOps_ until we receive a write event.
  // Note: references to elements in a deque are NOT invalidated by
  // insertion or deletion on either end of the deque (see std::deque).
  const auto& ref = writeOps_.back();
  auto handle = handle_;

  // Beware that we pass ref by reference for above reason.
  // If it were a copy, the preamble would be invalid on lambda return.
  device_->defer([handle, &ref] {
    handle->write((char*)&ref.preamble, sizeof(ref.preamble));

    // Also write buffer if applicable.
    if (ref.getOpcode() == Op::SEND_UNBOUND_BUFFER) {
      // Note: this non owning pointer will go out of scope before the
      // write has completed. In a failure scenario where the unbound
      // buffer is destructed before this write completes, it can
      // point to garbage and wreak havoc.
      handle->write((char*)ref.buf->ptr + ref.offset, ref.length);
    }
  });
}

// Send notification to peer that there is a pending send operation.
//
// Threading: called from either user thread or event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::sendNotifySendReady(uint64_t tag, size_t length) {
  Op op;
  op.preamble.nbytes = sizeof(op.preamble);
  op.preamble.opcode = Op::NOTIFY_SEND_READY;
  op.preamble.tag = tag;
  op.preamble.length = length;
  writeOp(std::move(op));
}

// Send notification to peer that there is a pending recv operation.
//
// Threading: called from either user thread or event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::sendNotifyRecvReady(uint64_t tag, size_t length) {
  Op op;
  op.preamble.nbytes = sizeof(op.preamble);
  op.preamble.opcode = Op::NOTIFY_RECV_READY;
  op.preamble.tag = tag;
  op.preamble.length = length;
  writeOp(std::move(op));
}

// Send unbound buffer to peer.
//
// Threading: called from either user thread or event loop thread.
// Locking requirements: caller must hold instance mutex.
//
void Pair::sendUnboundBuffer(
    uint64_t tag,
    NonOwningPtr<UnboundBuffer> buf,
    size_t offset,
    size_t length) {
  Op op;
  op.preamble.nbytes = sizeof(op.preamble) + length;
  op.preamble.opcode = Op::SEND_UNBOUND_BUFFER;
  op.preamble.tag = tag;
  op.preamble.length = length;
  op.buf = std::move(buf);
  op.offset = offset;
  op.length = length;
  writeOp(std::move(op));
}

// Send from the specified buffer to remote side of pair.
void Pair::send(
    transport::UnboundBuffer* tbuf,
    uint64_t tag,
    size_t offset,
    size_t nbytes) {
  auto buf = static_cast<UnboundBuffer*>(tbuf)->getWeakNonOwningPtr();

  if (nbytes > 0) {
    GLOO_ENFORCE_LE(offset, tbuf->size);
    GLOO_ENFORCE_LE(nbytes, tbuf->size - offset);
  }

  std::unique_lock<std::mutex> lock(mutex_);

  // TODO(check if the pair is in the right state)

  // Execute this send if there is a remote pending receive.
  Context::Mutator mutator(*context_, tag, rank_);
  if (mutator.shiftRemotePendingRecv()) {
    // We keep a count of remote pending send and receive operations.
    // In this code path the remote side hasn't seen a notification
    // for this send operation yet so we need to take special care
    // their count is updated regardless.
    sendNotifySendReady(tag, nbytes);
    sendUnboundBuffer(tag, NonOwningPtr<UnboundBuffer>(buf), offset, nbytes);
    return;
  }

  // Notify peer of this pending send.
  localPendingSend_[tag].emplace_back(std::move(buf), offset, nbytes);
  sendNotifySendReady(tag, nbytes);
}

// Receive into the specified buffer from the remote side of pair.
void Pair::recv(
    transport::UnboundBuffer* tbuf,
    uint64_t tag,
    size_t offset,
    size_t nbytes) {
  auto buf = static_cast<UnboundBuffer*>(tbuf)->getWeakNonOwningPtr();

  if (nbytes > 0) {
    GLOO_ENFORCE_LE(offset, tbuf->size);
    GLOO_ENFORCE_LE(nbytes, tbuf->size - offset);
  }

  std::unique_lock<std::mutex> lock(mutex_);

  // If this recv happens before the send notification,
  // we are still owed a send notification. Because this recv
  // has already been posted, we have to make sure it doesn't
  // hit the context wide tally.
  Context::Mutator mutator(*context_, tag, rank_);
  if (!mutator.shiftRemotePendingSend()) {
    mutator.pushExpectedSendNotification();
  }

  // Notify peer of this pending recv.
  localPendingRecv_[tag].emplace_back(std::move(buf), offset, nbytes);
  sendNotifyRecvReady(tag, nbytes);
}

bool Pair::tryRecv(
    transport::UnboundBuffer* tbuf,
    uint64_t tag,
    size_t offset,
    size_t nbytes) {
  auto buf = static_cast<UnboundBuffer*>(tbuf)->getWeakNonOwningPtr();

  if (nbytes > 0) {
    GLOO_ENFORCE_LE(offset, tbuf->size);
    GLOO_ENFORCE_LE(nbytes, tbuf->size - offset);
  }

  std::unique_lock<std::mutex> lock(mutex_);

  // todo check if state is right...
  //    throwIfException();

  // Return early if there is no remote pending send.
  Context::Mutator mutator(*context_, tag, rank_);
  if (!mutator.shiftRemotePendingSend()) {
    return false;
  }

  // Notify peer of this pending recv.
  localPendingRecv_[tag].emplace_back(std::move(buf), offset, nbytes);
  sendNotifyRecvReady(tag, nbytes);
  return true;
}

void Pair::closeWhileHoldingPairLock() {
  switch (state_) {
    case INITIALIZED:
      state_ = CLOSED;
      break;
    case CONNECTING:
      GLOO_ENFORCE_NE(
          state_, CONNECTING, "Cannot close pair while waiting on connection");
      break;
    case CONNECTED:
      device_->defer([=] { this->handle_->close(); });
      state_ = CLOSING;
      break;
    case CLOSING:
      // Nothing to do but wait...
      break;
    case CLOSED:
      // Already closed...
      break;
  }
}

void Pair::close() {
  std::unique_lock<std::mutex> lock(mutex_);
  closeWhileHoldingPairLock();
}

} // namespace uv
} // namespace transport
} // namespace gloo
