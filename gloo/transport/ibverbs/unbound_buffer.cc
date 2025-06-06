/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/ibverbs/unbound_buffer.h"

#include <cstring>

#include "gloo/common/error.h"
#include "gloo/common/logging.h"
#include "gloo/transport/ibverbs/context.h"

namespace gloo {
namespace transport {
namespace ibverbs {

UnboundBuffer::UnboundBuffer(
    const std::shared_ptr<Context>& context,
    void* ptr,
    size_t size)
    : ::gloo::transport::UnboundBuffer(ptr, size),
      context_(context),
      recvRank_(-1),
      sendCompletions_(0),
      sendRank_(-1),
      shareableNonOwningPtr_(this) {
  std::unique_lock<std::mutex> lock(m_);
  auto dev = context->device_;

  auto mr = ibv_reg_mr(
      dev->pd_,
      // Empty buffers still need a valid pointer and positive length otherwise
      // IB throws an error. We use our size 1 empty buffer for this.
      size == 0 ? static_cast<void*>(&emptyBuf_) : ptr,
      size == 0 ? sizeof(emptyBuf_) : size,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
          IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);

  remoteKey_.emplace(context_->rank, mr->addr, size, mr->lkey);

  // Provide hint if the error is EFAULT and nv_peer_mem is not loaded
  if (mr == nullptr && errno == EFAULT) {
    if (!dev->hasNvPeerMem_) {
      GLOO_ENFORCE(
          mr != nullptr,
          "ibv_reg_mr: ",
          strerror(errno),
          " (kernel module 'nv_peer_mem' not loaded;"
          " did you specify a pointer to GPU memory?)");
    }
  }

  // Provide hint if the error is ENOMEM
  if (mr == nullptr && errno == ENOMEM) {
    GLOO_ENFORCE(
        mr != nullptr,
        "ibv_reg_mr: ",
        strerror(errno),
        " (did you run into the locked memory limit?)");
  }

  GLOO_ENFORCE(
      mr != nullptr,
      "ibv_reg_mr: ",
      strerror(errno),
      " ptr=",
      ptr,
      " size=",
      size);

  mr_ = mr;
}

UnboundBuffer::~UnboundBuffer() {
  std::lock_guard<std::mutex> guard(m_);

  ibv_dereg_mr(mr_);
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

void UnboundBuffer::handleCompletion(int rank, struct ibv_wc* wc) {
  if (wc->opcode & IBV_WC_RECV) {
    std::unique_lock<std::mutex> lock(m_);
    recvCompletions_.emplace_back(rank);
    recvCv_.notify_one();
  } else if (
      wc->opcode == IBV_WC_RDMA_WRITE || wc->opcode == IBV_WC_RDMA_READ) {
    std::unique_lock<std::mutex> lock(m_);
    sendCompletions_++;
    sendPending_--;
    sendCv_.notify_one();

    GLOO_DEBUG(
        "send complete sendPending=",
        sendPending_,
        " sendCompletions=",
        sendCompletions_);
  } else {
    GLOO_ENFORCE(false, "Unexpected completion (opcode: ", wc->opcode, ")");
  }
}

bool UnboundBuffer::waitRecv(int* rank, std::chrono::milliseconds timeout) {
  // The device thread will signal completion. If the completion
  // hasn't arrived yet, wait until it does.
  auto pred = [&] {
    throwIfException();
    return abortWaitRecv_ || recvCompletions_.size() > 0;
  };
  std::unique_lock<std::mutex> lock(m_);
  if (timeout == kNoTimeout || timeout == kUnsetTimeout) {
    // No timeout set. Wait for read to complete.
    recvCv_.wait(lock, pred);
  } else {
    auto done = recvCv_.wait_for(lock, timeout, pred);
    if (!done) {
      // Release the mutex before calling into the pair to avoid deadlock.
      // Calling signalIoFailure() will throw, so no need to
      // reacquire.
      lock.unlock();

      auto msg = GLOO_ERROR_MSG(
          "Timed out waiting ",
          timeout.count(),
          "ms for recv operation to complete");
      context_->signalException(msg);
      throw ::gloo::IoException(msg);
    }
  }

  if (abortWaitRecv_) {
    // Reset to false, so that only this waitRecv is interrupted
    abortWaitRecv_ = false;
    return false;
  }

  if (rank != nullptr) {
    *rank = recvCompletions_.front();
  }
  recvCompletions_.pop_front();

  return true;
}

bool UnboundBuffer::waitSend(int* rank, std::chrono::milliseconds timeout) {
  // The device thread will signal completion. If the completion
  // hasn't arrived yet, wait until it does.
  std::unique_lock<std::mutex> lock(m_);
  throwIfException();
  if (sendCompletions_ == 0) {
    GLOO_ENFORCE_GT(sendPending_, 0, "No send to wait for");
    auto pred = [&] {
      throwIfException();
      return abortWaitSend_ || sendCompletions_ > 0;
    };
    if (timeout == kNoTimeout || timeout == kUnsetTimeout) {
      // No timeout set. Wait for read to complete.
      sendCv_.wait(lock, pred);
    } else {
      auto done = sendCv_.wait_for(lock, timeout, pred);
      if (!done) {
        // Release the mutex before calling into the pair to avoid deadlock.
        // Calling signalIoFailure() will throw, so no need to
        // reacquire.
        lock.unlock();
        auto msg = GLOO_ERROR_MSG(
            "Timed out waiting ",
            timeout.count(),
            "ms for send operation to complete");
        context_->signalException(msg);
        throw ::gloo::IoException(msg);
      }
    }
  }

  if (abortWaitSend_) {
    // Reset to false, so that only this waitSend is interrupted
    abortWaitSend_ = false;
    return false;
  }
  sendCompletions_--;
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
  sendPending_++;
  context_->getPair(dstRank)->send(this, slot, offset, nbytes);
}

void UnboundBuffer::put(
    const transport::RemoteKey& remoteKey,
    uint64_t slot,
    size_t offset,
    size_t roffset,
    size_t nbytes) {
  sendPending_++;
  context_->getPair(remoteKey.rank)
      ->put(this, remoteKey, slot, offset, roffset, nbytes);
}

void UnboundBuffer::get(
    const transport::RemoteKey& remoteKey,
    uint64_t slot,
    size_t offset,
    size_t roffset,
    size_t nbytes) {
  sendPending_++;
  context_->getPair(remoteKey.rank)
      ->get(this, remoteKey, slot, offset, roffset, nbytes);
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

  GLOO_ENFORCE_EQ(srcRanks.size(), 1, "TODO: Only one src rank is supported");

  for (auto rank : srcRanks) {
    context_->getPair(rank)->recv(this, slot, offset, nbytes);
  }
}

void UnboundBuffer::signalError(const std::exception_ptr& ex) {
  std::lock_guard<std::mutex> lock(m_);
  ex_ = ex;
  recvCv_.notify_all();
  sendCv_.notify_all();
}

void UnboundBuffer::throwIfException() {
  if (ex_ != nullptr) {
    std::rethrow_exception(ex_);
  }
}

std::unique_ptr<::gloo::transport::RemoteKey> UnboundBuffer::getRemoteKey()
    const {
  return std::make_unique<RemoteKey>(remoteKey_.value());
}

} // namespace ibverbs
} // namespace transport
} // namespace gloo
