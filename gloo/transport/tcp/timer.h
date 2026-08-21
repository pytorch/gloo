/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

#include <gloo/transport/tcp/loop.h>

namespace gloo {
namespace transport {
namespace tcp {

// One-shot timers integrated with the TCP epoll loop.
class Timer final : public Handler, public std::enable_shared_from_this<Timer> {
 public:
  using function_t = std::function<void()>;

  Timer(std::shared_ptr<Loop> loop, function_t fn);

  ~Timer() override;

  void schedule(std::chrono::milliseconds timeout);

  void cancel();

  void handleEvents(Loop& loop, int events) override;

 private:
  void cleanup();

  std::weak_ptr<Loop> loop_;
  function_t fn_;
  std::atomic<int> fd_{-1};
  std::atomic<bool> canceled_{false};
  std::atomic<bool> armed_{false};
};

} // namespace tcp
} // namespace transport
} // namespace gloo
