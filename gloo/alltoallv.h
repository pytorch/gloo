/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/common/enforce.h"
#include "gloo/context.h"
#include "gloo/transport/unbound_buffer.h"

namespace gloo {

class AlltoallvOptions {
 public:
  explicit AlltoallvOptions(const std::shared_ptr<Context>& context)
      : context(context), timeout(context->getTimeout()) {}

  template <typename T>
  void setInput(
      std::unique_ptr<transport::UnboundBuffer> buf,
      std::vector<int64_t> elementsPerRank) {
    setInput(std::move(buf), std::move(elementsPerRank), sizeof(T));
  }

  template <typename T>
  void setInput(T* ptr, std::vector<int64_t> elementsPerRank) {
    setInput(static_cast<void*>(ptr), std::move(elementsPerRank), sizeof(T));
  }

  template <typename T>
  void setOutput(
      std::unique_ptr<transport::UnboundBuffer> buf,
      std::vector<int64_t> elementsPerRank) {
    setOutput(std::move(buf), std::move(elementsPerRank), sizeof(T));
  }

  template <typename T>
  void setOutput(T* ptr, std::vector<int64_t> elementsPerRank) {
    setOutput(static_cast<void*>(ptr), std::move(elementsPerRank), sizeof(T));
  }

  void setTag(uint32_t tag_2) {
    this->tag = tag_2;
  }

  void setTimeout(std::chrono::milliseconds timeout_2) {
    GLOO_ENFORCE(timeout_2.count() > 0);
    this->timeout = timeout_2;
  }

 protected:
  std::shared_ptr<Context> context;
  std::unique_ptr<transport::UnboundBuffer> in;
  std::unique_ptr<transport::UnboundBuffer> out;
  std::vector<size_t> inOffsetPerRank;
  std::vector<size_t> inLengthPerRank;
  std::vector<size_t> outOffsetPerRank;
  std::vector<size_t> outLengthPerRank;

  // Number of bytes per element.
  size_t elementSize = 0;

  // Tag for this operation.
  // Must be unique across operations executing in parallel.
  uint32_t tag = 0;

  // Set element size, or check the argument is equal to the current value.
  void setElementSize(size_t elementSize);

  // Untemplated implementation of setInput on unbound buffer.
  void setInput(
      std::unique_ptr<transport::UnboundBuffer> buf,
      std::vector<int64_t> elementsPerRank,
      size_t elementSize);

  // Untemplated implementation of setInput on opaque pointer.
  void setInput(
      void* ptr,
      std::vector<int64_t> elementsPerRank,
      size_t elementSize);

  // Untemplated implementation of setOutput on unbound buffer.
  void setOutput(
      std::unique_ptr<transport::UnboundBuffer> buf,
      std::vector<int64_t> elementsPerRank,
      size_t elementSize);

  // Untemplated implementation of setOutput on opaque pointer.
  void setOutput(
      void* ptr,
      std::vector<int64_t> elementsPerRank,
      size_t elementSize);

  // End-to-end timeout for this operation.
  std::chrono::milliseconds timeout;

  friend void alltoallv(AlltoallvOptions&);
};

void alltoallv(AlltoallvOptions& opts);

} // namespace gloo
