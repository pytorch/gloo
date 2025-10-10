/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <iostream>

#include "gloo/common/enforce.h"
#include "gloo/common/error.h"
#include "gloo/common/log.h"
#include "gloo/transport/tcp/buffer.h"

namespace gloo {
namespace transport {
namespace tcp {

Buffer::Buffer(Pair* pair, int slot, void* ptr, size_t size)
    : ::gloo::transport::Buffer(slot, ptr, size),
      pair_(pair),
      recvCompletions_(0),
      sendCompletions_(0),
      sendPending_(0),
      ex_(nullptr) {}

Buffer::~Buffer() {
  pair_->unregisterBuffer(this);
}

void Buffer::handleRecvCompletion() {
  std::lock_guard<std::mutex> lock(m_);
  recvCompletions_++;
  recvCv_.notify_one();
}

void Buffer::waitRecv() {
  // If the pair is in synchronous mode, the current thread is
  // responsible for doing reads.
  // Since a single pair potentially serves multiple buffers, a
  // read may be intended for another buffer.
  if (pair_->isSync()) {
    // We can assume a single pair is never used by more than one
    // thread, so there is no need to acquire the mutex here.
    while (recvCompletions_ == 0) {
      pair_->recv();
    }
    recvCompletions_--;
  } else {
    // The device thread will signal completion. If the completion
    // hasn't arrived yet, wait until it does or read times out.
    auto timeout = pair_->getTimeout();
    auto pred = [&] {
      throwIfException();
      return recvCompletions_ > 0;
    };
    std::unique_lock<std::mutex> lock(m_);
    if (timeout == kNoTimeout) {
      // No timeout set. Wait for read to complete.
      recvCv_.wait(lock, pred);
    } else {
      auto done = recvCv_.wait_for(lock, timeout, pred);
      if (!done) {
        // Release the mutex before calling into the pair to avoid deadlock.
        lock.unlock();
        std::rethrow_exception(pair_->signalExceptionExternal(
            GLOO_ERROR_MSG("Read timeout ", pair_->peer().str())));
      }
    }
    recvCompletions_--;
  }
}

void Buffer::handleSendCompletion() {
  std::lock_guard<std::mutex> lock(m_);
  sendCompletions_++;
  sendPending_--;
  sendCv_.notify_one();
}

void Buffer::waitSend() {
  if (pair_->isSync()) {
    // The send operation must flush all data to the underlying socket
    // and then call handleSendCompletion. Therefore, the number of
    // send completions must always be positive when calling waitSend.
    GLOO_ENFORCE_GE(1, sendCompletions_);
    sendCompletions_--;
  } else {
    // The device thread will signal completion. If the completion
    // hasn't arrived yet, wait until it does or write times out.
    auto timeout = pair_->getTimeout();
    auto pred = [&] {
      throwIfException();
      return sendCompletions_ > 0;
    };
    std::unique_lock<std::mutex> lock(m_);
    if (sendCompletions_ == 0) {
      GLOO_ENFORCE_GT(sendPending_, 0, "No send to wait for");
      if (timeout == kNoTimeout) {
        // No timeout set. Wait for write to complete.
        sendCv_.wait(lock, pred);
      } else {
        auto done = sendCv_.wait_for(lock, timeout, pred);
        if (!done) {
          // Release the mutex before calling into the pair to avoid deadlock.
          lock.unlock();
          std::rethrow_exception(pair_->signalExceptionExternal(
              GLOO_ERROR_MSG("Write timeout ", pair_->peer().str())));
        }
      }
    }
    sendCompletions_--;
  }
}

void Buffer::send(size_t offset, size_t length, size_t roffset) {
  Op op;

  // Can't assert on roffset, since we don't know the size of
  // the remote buffer. Refactor of initialization code needed
  // to support this.
  GLOO_ENFORCE_LE(offset + length, size_);

  GLOO_TRACE("send {} bytes to {}", length, pair_->peer().str());

  op.preamble.nbytes = sizeof(op.preamble) + length;
  op.preamble.opcode = Op::SEND_BUFFER;
  op.preamble.slot = slot_;
  op.preamble.offset = offset;
  op.preamble.length = length;
  op.preamble.roffset = roffset;
  op.buf = this;

  // Increment number of sends in flight
  sendPending_++;

  // Pass to pair
  pair_->send(op);
}

void Buffer::signalException(std::exception_ptr ex) {
  std::lock_guard<std::mutex> lock(m_);
  ex_ = std::move(ex);
  recvCv_.notify_all();
  sendCv_.notify_all();
}

void Buffer::throwIfException() {
  if (ex_ != nullptr) {
    std::rethrow_exception(ex_);
  }
}

} // namespace tcp
} // namespace transport
} // namespace gloo
