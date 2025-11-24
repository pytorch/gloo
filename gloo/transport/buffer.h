/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stddef.h>

namespace gloo {
namespace transport {

class Buffer {
 public:
  explicit Buffer(int slot, void* ptr, size_t size)
      : slot_(slot), ptr_(ptr), size_(size) {}
  virtual ~Buffer() = 0;

  virtual void send(size_t offset, size_t length, size_t roffset = 0) = 0;

  // Send entire buffer by default
  void send() {
    send(0, size_);
  }

  virtual void waitRecv() = 0;
  virtual void waitSend() = 0;

 protected:
  int slot_;
  void* ptr_;
  size_t size_;
};

} // namespace transport
} // namespace gloo
