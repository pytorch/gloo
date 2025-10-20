/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/algorithm.h"
#include "gloo/context.h"
#include "gloo/transport/unbound_buffer.h"

namespace gloo {

class Barrier : public Algorithm {
 public:
  explicit Barrier(const std::shared_ptr<Context>& context)
      : Algorithm(context) {}

  virtual ~Barrier() {}
};

class BarrierOptions {
 public:
  explicit BarrierOptions(const std::shared_ptr<Context>& context);

  void setTag(uint32_t tag_2) {
    this->tag = tag_2;
  }

  void setTimeout(std::chrono::milliseconds timeout_2) {
    this->timeout = timeout_2;
  }

 protected:
  std::shared_ptr<Context> context;
  std::unique_ptr<transport::UnboundBuffer> buffer;

  // Tag for this operation.
  // Must be unique across operations executing in parallel.
  uint32_t tag = 0;

  // End-to-end timeout for this operation.
  std::chrono::milliseconds timeout;

  friend void barrier(BarrierOptions&);
};

void barrier(BarrierOptions& opts);

} // namespace gloo
