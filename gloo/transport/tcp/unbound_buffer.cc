/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/tcp/unbound_buffer.h"

#include "gloo/common/enforce.h"
#include "gloo/common/error.h"
#include "gloo/transport/tcp/context.h"

namespace gloo {
namespace transport {
namespace tcp {

UnboundBuffer::UnboundBuffer(
    const std::shared_ptr<Context>& context,
    void* ptr,
    size_t size)
    : ::gloo::transport::UnboundBuffer(ptr, size),
      context_(context),
      recvCompletions_(0),
      recvRank_(-1),
      sendCompletions_(0),
      sendRank_(-1),
      shareableNonOwningPtr_(this) {}

UnboundBuffer::~UnboundBuffer() {}

void UnboundBuffer::handleRecvCompletion(int rank) {
  std::lock_guard<std::mutex> lock(m_);
  recvCompletions_++;
  recvRank_ = rank;
  recvCv_.notify_one();
}

void UnboundBuffer::abortWaitRecv() {
  std::lock_guard<std::mutex> guard(m_);
  abortWaitRecv_ = true;
  recvCv_.notify_one();
}

void UnboundBuffer::abortWaitSend() {
  std::lock_guard<std::mutex> guard(m_);
  abortWaitSend_ = true;
  sendCv_.notify_one();
}

bool UnboundBuffer::waitRecv(int* rank, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(m_);
  if (timeout == kUnsetTimeout) {
    timeout = context_->getTimeout();
  }

  if (recvCompletions_ == 0) {
    auto done = recvCv_.wait_for(lock, timeout, [&] {
      throwIfException();
      return abortWaitRecv_ || recvCompletions_ > 0;
    });
    if (!done) {
      // Below, we let all pairs in the transport context know about this
      // application side timeout. This in turn will call into all pending
      // operations to let them know about the error. This includes the
      // operation that is pending for this buffer, so in order for a call to
      // this instance its 'signalException' function to not deadlock, we need
      // to first release the instance wide lock.
      lock.unlock();

      // Signal all pairs about this application timeout.
      // Note that the exception that they see indicates it was another
      // operation that timed out. This this exception surfaces anywhere,n
      // be sure to look for the actual cause (seen below).
      context_->signalException("Application timeout caused pair closure");

      throw ::gloo::IoException(GLOO_ERROR_MSG(
          "Timed out waiting ",
          timeout.count(),
          "ms for recv operation to complete"));
    }
  }
  if (abortWaitRecv_) {
    // Reset to false, so that only this waitRecv is interrupted
    abortWaitRecv_ = false;
    return false;
  }
  recvCompletions_--;
  if (rank != nullptr) {
    *rank = recvRank_;
  }
  return true;
}

void UnboundBuffer::handleSendCompletion(int rank) {
  std::lock_guard<std::mutex> lock(m_);
  sendCompletions_++;
  sendRank_ = rank;
  sendCv_.notify_one();
}

bool UnboundBuffer::waitSend(int* rank, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(m_);
  if (timeout == kUnsetTimeout) {
    timeout = context_->getTimeout();
  }

  if (sendCompletions_ == 0) {
    auto done = sendCv_.wait_for(lock, timeout, [&] {
      throwIfException();
      return abortWaitSend_ || sendCompletions_ > 0;
    });
    if (!done) {
      // Below, we let all pairs in the transport context know about this
      // application side timeout. This in turn will call into all pending
      // operations to let them know about the error. This includes the
      // operation that is pending for this buffer, so in order for a call to
      // this instance its 'signalException' function to not deadlock, we need
      // to first release the instance wide lock.
      lock.unlock();

      // Signal all pairs about this application timeout.
      // Note that the exception that they see indicates it was another
      // operation that timed out. This this exception surfaces anywhere,n
      // be sure to look for the actual cause (seen below).
      context_->signalException("Application timeout caused pair closure");

      throw ::gloo::IoException(GLOO_ERROR_MSG(
          "Timed out waiting ",
          timeout.count(),
          "ms for send operation to complete"));
    }
  }

  if (abortWaitSend_) {
    // Reset to false, so that only this waitSend is interrupted
    abortWaitSend_ = false;
    return false;
  }
  sendCompletions_--;
  if (rank != nullptr) {
    *rank = sendRank_;
  }
  return true;
}

void UnboundBuffer::send(
    int dstRank,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  // Default the number of bytes to be equal to the number
  // of bytes remaining in the buffer w.r.t. the offset.
  if (nbytes == kUnspecifiedByteCount) {
    GLOO_ENFORCE_LE(offset, this->size);
    nbytes = this->size - offset;
  }
  context_->getPair(dstRank)->send(this, slot, offset, nbytes);
}

void UnboundBuffer::recv(
    int srcRank,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  // Default the number of bytes to be equal to the number
  // of bytes remaining in the buffer w.r.t. the offset.
  if (nbytes == kUnspecifiedByteCount) {
    GLOO_ENFORCE_LE(offset, this->size);
    nbytes = this->size - offset;
  }
  context_->getPair(srcRank)->recv(this, slot, offset, nbytes);
}

void UnboundBuffer::recv(
    std::vector<int> srcRanks,
    uint64_t slot,
    size_t offset,
    size_t nbytes) {
  // Default the number of bytes to be equal to the number
  // of bytes remaining in the buffer w.r.t. the offset.
  if (nbytes == kUnspecifiedByteCount) {
    GLOO_ENFORCE_LT(offset, this->size);
    nbytes = this->size - offset;
  }
  context_->recvFromAny(this, slot, offset, nbytes, srcRanks);
}

void UnboundBuffer::signalException(std::exception_ptr ex) {
  std::lock_guard<std::mutex> lock(m_);
  ex_ = std::move(ex);
  recvCv_.notify_all();
  sendCv_.notify_all();
}

void UnboundBuffer::throwIfException() {
  if (ex_ != nullptr) {
    std::rethrow_exception(ex_);
  }
}

} // namespace tcp
} // namespace transport
} // namespace gloo
