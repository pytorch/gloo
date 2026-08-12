/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/tcp/timer.h>

#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <gloo/common/error.h>
#include <gloo/common/logging.h>

namespace gloo {
namespace transport {
namespace tcp {

Timer::Timer(std::shared_ptr<Loop> loop, function_t fn)
    : loop_(std::move(loop)), fn_(std::move(fn)) {
  fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  GLOO_ENFORCE_NE(fd_, -1, "timerfd_create: ", strerror(errno));
}

Timer::~Timer() {
  cleanup();
}

void Timer::schedule(std::chrono::milliseconds timeout) {
  GLOO_ENFORCE_GE(timeout.count(), 0);
  GLOO_ENFORCE(!armed_.exchange(true), "Timer is already armed");
  canceled_ = false;

  auto loop = loop_.lock();
  GLOO_ENFORCE(loop, "Loop is no longer available");
  loop->registerDescriptor(fd_, EPOLLIN, shared_from_this());

  struct itimerspec spec = {};
  auto interval = timeout;
  if (interval == std::chrono::milliseconds(0)) {
    interval = std::chrono::milliseconds(1);
  }
  spec.it_value.tv_sec =
      std::chrono::duration_cast<std::chrono::seconds>(interval).count();
  spec.it_value.tv_nsec =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          interval % std::chrono::seconds(1))
          .count();

  auto rv = timerfd_settime(fd_, 0, &spec, nullptr);
  GLOO_ENFORCE_NE(rv, -1, "timerfd_settime: ", strerror(errno));
}

void Timer::cancel() {
  canceled_ = true;
  if (!armed_) {
    return;
  }

  struct itimerspec spec = {};
  auto rv = timerfd_settime(fd_, 0, &spec, nullptr);
  GLOO_ENFORCE_NE(rv, -1, "timerfd_settime: ", strerror(errno));

  auto loop = loop_.lock();
  GLOO_ENFORCE(loop, "Loop is no longer available");
  if (loop->inLoopThread()) {
    // If the timer fd and a listener fd are returned in the same epoll_wait
    // batch, defer cleanup so a queued timer event cannot observe a closed fd.
    loop->defer([self = shared_from_this()] { self->cleanup(); });
    return;
  }

  cleanup();
}

void Timer::handleEvents(Loop&, int /* events */) {
  auto self = shared_from_this();

  uint64_t expirations = 0;
  auto rv = read(fd_, &expirations, sizeof(expirations));
  if (rv == -1 && errno == EINTR) {
    rv = read(fd_, &expirations, sizeof(expirations));
  }
  GLOO_ENFORCE_NE(rv, -1, "read: ", strerror(errno));

  auto canceled = canceled_.load();
  cleanup();
  if (!canceled) {
    fn_();
  }
}

void Timer::cleanup() {
  if (!armed_.exchange(false)) {
    return;
  }

  auto fd = fd_;
  if (fd == -1) {
    return;
  }

  if (auto loop = loop_.lock()) {
    loop->unregisterDescriptor(fd, this);
  }

  close(fd);
  fd_ = -1;
}

} // namespace tcp
} // namespace transport
} // namespace gloo
