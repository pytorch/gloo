/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/tcp/listener.h>

#include <netinet/tcp.h>
#include <string.h>
#include <vector>

#include <gloo/common/common.h>
#include <gloo/common/logging.h>
#include <gloo/common/utils.h>
#include <gloo/transport/tcp/helpers.h>
#include <gloo/transport/tcp/timer.h>

namespace gloo {
namespace transport {
namespace tcp {

Listener::Listener(std::shared_ptr<Loop> loop, const attr& attr)
    : loop_(std::move(loop)) {
  if (attr.ai_fd >= 0) {
    listener_ = std::make_shared<Socket>(attr.ai_fd);
    listener_->block(false);
  } else {
    listener_ = Socket::createForFamily(attr.ai_addr.ss_family);
    listener_->reuseAddr(true);
    listener_->bind(attr.ai_addr);
  }
  listener_->listen(kBacklog);
  addr_ = listener_->sockName();
  useRankAsSeqNumber_ = useRankAsSeqNumber();

  // Register with loop for readability events.
  loop_->registerDescriptor(listener_->fd(), EPOLLIN, this);
}

void Listener::shutdown() {
  if (*closed_) {
    return;
  }

  std::vector<connect_callback_t> callbacks;
  std::vector<std::shared_ptr<Timer>> timers;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    *closed_ = true;
    for (auto& it : seqToCallback_) {
      if (!it.second.resolved) {
        callbacks.push_back(std::move(it.second.fn));
      }
      if (it.second.timer) {
        timers.push_back(it.second.timer);
      }
    }
    seqToCallback_.clear();
    seqToSocket_.clear();
  }

  for (auto& timer : timers) {
    timer->cancel();
  }
  for (auto& fn : callbacks) {
    auto socket = std::shared_ptr<Socket>();
    auto error = LoopError("listener shut down while waiting for connection");
    fn(socket, error);
  }
  if (listener_) {
    loop_->unregisterDescriptor(listener_->fd(), this);
  }
}

Listener::~Listener() {
  shutdown();
}

void Listener::handleEvents(Loop& loop, int /* unused */) {
  std::lock_guard<std::mutex> guard(mutex_);

  for (;;) {
    auto sock = listener_->accept();
    if (!sock) {
      // Let the loop try again on the next tick.
      if (errno == EAGAIN) {
        return;
      }
      // Actual error.
      GLOO_ENFORCE(false, "accept: ", strerror(errno));
    }

    sock->reuseAddr(true);
    sock->noDelay(true);

    // Read sequence number.
    read<sequence_number_t>(
        loop,
        sock,
        [this, closed = closed_](
            std::shared_ptr<Socket> socket,
            const Error& error,
            sequence_number_t&& seq) {
          // If there was an error reading from the socket, the
          // sequence number will be bogus, and we can't route it to
          // the right callback. Ignore it.
          if (error) {
            return;
          }

          if (*closed) {
            return;
          }

          haveConnection(std::move(socket), seq);
        });
  }
}

Address Listener::nextAddress() {
  std::lock_guard<std::mutex> guard(mutex_);
  GLOO_ENFORCE(
      !useRankAsSeqNumber_,
      "Listener cannot use internal sequence with enabled option to use rank as sequence number");
  return Address(addr_.getSockaddr(), seq_++);
}

Address Listener::nextAddress(int seq) {
  GLOO_ENFORCE(
      useRankAsSeqNumber_,
      "Listener must be setup to use rank as sequence number");
  return Address(addr_.getSockaddr(), seq);
}

void Listener::waitForConnection(
    sequence_number_t seq,
    std::chrono::milliseconds timeout,
    connect_callback_t fn) {
  std::unique_lock<std::mutex> lock(mutex_);

  // If we don't yet have an fd for this sequence number, persist callback.
  auto it = seqToSocket_.find(seq);
  if (it == seqToSocket_.end()) {
    auto pendingIt = seqToCallback_.find(seq);
    if (pendingIt != seqToCallback_.end()) {
      if (pendingIt->second.resolved) {
        seqToCallback_.erase(pendingIt);
      } else {
        GLOO_ENFORCE(
            false,
            "Duplicate waitForConnection for sequence number ",
            std::to_string(seq));
      }
    }

    PendingConnection pending{
        std::move(fn),
        nullptr,
        false,
    };
    if (timeout != kNoTimeout) {
      pending.timer =
          loop_->createTimer([this, seq] { timeoutConnection(seq); });
      pending.timer->schedule(timeout);
    }
    seqToCallback_.emplace(seq, std::move(pending));
    return;
  }

  // If we already have an fd for this sequence number, schedule
  // the callback.
  auto socket = std::move(it->second);
  seqToSocket_.erase(it);
  loop_->defer([fn, socket]() { fn(socket, Error::kSuccess); });
}

void Listener::haveConnection(
    std::shared_ptr<Socket> socket,
    sequence_number_t seq) {
  std::unique_lock<std::mutex> lock(mutex_);

  // If we don't yet have a callback for this sequence number,
  // persist the socket.
  auto it = seqToCallback_.find(seq);
  if (it == seqToCallback_.end()) {
    seqToSocket_.emplace(seq, std::move(socket));
    return;
  }

  // If the wait for this sequence number already timed out, drop the late
  // socket instead of stashing it again.
  if (it->second.resolved) {
    seqToCallback_.erase(it);
    return;
  }

  // If we already have a callback for this sequence number, trigger it.
  auto fn = std::move(it->second.fn);
  auto timer = std::move(it->second.timer);
  it->second.resolved = true;
  seqToCallback_.erase(it);
  lock.unlock();
  if (timer) {
    timer->cancel();
  }
  // Keep success callbacks on the loop thread.
  auto complete = [fn = std::move(fn), socket = std::move(socket)]() mutable {
    fn(std::move(socket), Error::kSuccess);
  };
  loop_->defer(std::move(complete));
}

void Listener::timeoutConnection(sequence_number_t seq) {
  connect_callback_t fn;
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = seqToCallback_.find(seq);
  if (*closed_ || it == seqToCallback_.end() || it->second.resolved) {
    return;
  }

  // Leave the resolved entry in place so that a late socket for the same
  // sequence number can be detected and dropped in haveConnection.
  it->second.resolved = true;
  it->second.timer.reset();
  fn = std::move(it->second.fn);
  lock.unlock();

  auto socket = std::shared_ptr<Socket>();
  auto error = TimeoutError(
      "timed out waiting for connection with sequence number " +
      std::to_string(seq));
  fn(socket, error);
}

} // namespace tcp
} // namespace transport
} // namespace gloo
