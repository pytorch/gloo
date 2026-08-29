/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/uv/unbound_buffer.h>

#include <gloo/common/error.h>
#include <gloo/common/logging.h>
#include <gloo/transport/uv/context.h>

namespace gloo {
namespace transport {
namespace uv {

UnboundBuffer::UnboundBuffer(
    std::shared_ptr<Context> context,
    void* ptr,
    size_t size)
    : ::gloo::transport::UnboundBuffer(ptr, size),
      context_(context),
      recvCompletions_(0),
      recvRank_(-1),
      sendCompletions_(0),
      sendRank_(-1),
      shareableNonOwningPtr_(this) {
  gloo::_register_cv(&recvCv_);
  gloo::_register_cv(&sendCv_);
}

UnboundBuffer::~UnboundBuffer() {
  gloo::_deregister_cv(&recvCv_);
  gloo::_deregister_cv(&sendCv_);
}

void UnboundBuffer::handleRecvCompletion(int rank) {
  std::lock_guard<std::mutex> lock(mutex_);
  recvCompletions_++;
  recvRank_ = rank;
  recvCv_.notify_one();
}

void UnboundBuffer::abortWaitRecv() {
  std::lock_guard<std::mutex> guard(mutex_);
  abortWaitRecv_ = true;
  recvCv_.notify_one();
}

void UnboundBuffer::abortWaitSend() {
  std::lock_guard<std::mutex> guard(mutex_);
  abortWaitSend_ = true;
  sendCv_.notify_one();
}

bool UnboundBuffer::waitRecv(int* rank, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (timeout == kUnsetTimeout) {
    timeout = context_->getTimeout();
  }

  if (recvCompletions_ == 0) {
    auto done = recvCv_.wait_for(lock, timeout, [&] {
        if(gloo::_is_aborted()) {
          abortWaitRecv_ = true;
        }
       return abortWaitRecv_ || recvCompletions_ > 0;
    });
    if (!done) {
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
  std::lock_guard<std::mutex> lock(mutex_);
  sendCompletions_++;
  sendRank_ = rank;
  sendCv_.notify_one();
}

bool UnboundBuffer::waitSend(int* rank, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (timeout == kUnsetTimeout) {
    timeout = context_->getTimeout();
  }

  if (sendCompletions_ == 0) {
    auto done = sendCv_.wait_for(lock, timeout, [&] {
       if(gloo::_is_aborted()) {
         abortWaitSend_ = true;
       }
       return abortWaitSend_ || sendCompletions_ > 0;
    });
    if (!done) {
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
  if (nbytes == UINT64_MAX) {
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
  if (nbytes == UINT64_MAX) {
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
  if (nbytes == UINT64_MAX) {
    GLOO_ENFORCE_LT(offset, this->size);
    nbytes = this->size - offset;
  }
  context_->recvFromAny(this, slot, offset, nbytes, srcRanks);
}

} // namespace uv
} // namespace transport
} // namespace gloo
