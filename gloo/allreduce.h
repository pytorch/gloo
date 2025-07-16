/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <type_traits>

#include "gloo/context.h"
#include "gloo/transport/unbound_buffer.h"
#include "gloo/types.h"
//#include "gloo/allreduce_shm.h"

#define GPF_PRINT(...) do {\
    printf("GPF_DEBUG:");\
    printf(__VA_ARGS__);\
    printf("\n");\
}while(0)

namespace gloo {

namespace detail {

struct AllreduceOptionsImpl {
  // This type describes the function to use for element wise reduction.
  //
  // Its arguments are:
  //   1. non-const output pointer
  //   2. const input pointer 1 (may be equal to 1)
  //   3. const input pointer 2 (may be equal to 1)
  //   4. number of elements to reduce.
  //
  // Note that this function is not strictly typed and takes void pointers.
  // This is specifically done to avoid the need for a templated options class
  // and templated algorithm implementations. We found this adds very little
  // value for the increase in compilation time and code size.
  //
  using Func = std::function<void(void*, const void*, const void*, size_t)>;

#if GLOO_USE_TORCH_DTYPES
using BFloat16 = c10::BFloat16;
using Half = c10::Half;
#endif

  enum Algorithm {
    UNSPECIFIED = 0,
    RING = 1,
    BCUBE = 2,
  };

  enum ScalarType {
    BFLOAT16,
    HALF,
    FLOAT,
    UNKNOWN,
  };

  explicit AllreduceOptionsImpl(const std::shared_ptr<Context>& context)
      : context(context),
        timeout(context->getTimeout()),
        algorithm(UNSPECIFIED) {}

  std::shared_ptr<Context> context;

  // End-to-end timeout for this operation.
  std::chrono::milliseconds timeout;

  // Algorithm selection.
  Algorithm algorithm;

  // Scalar type
  ScalarType scalarType;

  // Input and output buffers.
  // The output is used as input if input is not specified.
  std::vector<std::unique_ptr<transport::UnboundBuffer>> in;
  std::vector<std::unique_ptr<transport::UnboundBuffer>> out;

  // Number of elements.
  size_t elements = 0;

  // Number of bytes per element.
  size_t elementSize = 0;

  // Reduction function.
  Func reduce;

  // Tag for this operation.
  // Must be unique across operations executing in parallel.
  uint32_t tag = 0;

  // This is the maximum size of each I/O operation (send/recv) of which
  // two are in flight at all times. A smaller value leads to more
  // overhead and a larger value leads to poor cache behavior.
  static constexpr size_t kMaxSegmentSize = 1024 * 1024;

  // Internal use only. This is used to exercise code paths where we
  // have more than 2 segments per rank without making the tests slow
  // (because they would require millions of elements if the default
  // were not configurable).
  size_t maxSegmentSize = kMaxSegmentSize;
};

} // namespace detail

class AllreduceOptions {
 public:
  using Func = detail::AllreduceOptionsImpl::Func;
  using Algorithm = detail::AllreduceOptionsImpl::Algorithm;
  using ScalarType = detail::AllreduceOptionsImpl::ScalarType;

  explicit AllreduceOptions(const std::shared_ptr<Context>& context)
      : impl_(context) {}

  void setAlgorithm(Algorithm algorithm) {
    impl_.algorithm = algorithm;
  }

  template <typename T>
  void setInput(std::unique_ptr<transport::UnboundBuffer> buf) {
    std::vector<std::unique_ptr<transport::UnboundBuffer>> bufs(1);
    bufs[0] = std::move(buf);
    setInputs<T>(std::move(bufs));
  }

  template <typename T>
  void setInputs(std::vector<std::unique_ptr<transport::UnboundBuffer>> bufs) {
    impl_.elements = bufs[0]->size / sizeof(T);
    impl_.elementSize = sizeof(T);
    impl_.in = std::move(bufs);
  }

  template <typename T>
  void setInput(T* ptr, size_t elements) {
    setInputs(&ptr, 1, elements);
  }

  template <typename T>
  void setInputs(std::vector<T*> ptrs, size_t elements) {
    setInputs(ptrs.data(), ptrs.size(), elements);
  }

  template <typename T>
  void setInputs(T** ptrs, size_t len, size_t elements) {
    impl_.elements = elements;
    impl_.elementSize = sizeof(T);
    impl_.in.reserve(len);
    for (size_t i = 0; i < len; i++) {
      impl_.in.push_back(
          impl_.context->createUnboundBuffer(ptrs[i], elements * sizeof(T)));
    }
  }

  template <typename T>
  void setOutput(std::unique_ptr<transport::UnboundBuffer> buf) {
    std::vector<std::unique_ptr<transport::UnboundBuffer>> bufs(1);
    bufs[0] = std::move(buf);
    setOutputs<T>(std::move(bufs));
  }

  template <typename T>
  void setOutputs(std::vector<std::unique_ptr<transport::UnboundBuffer>> bufs) {
    impl_.elements = bufs[0]->size / sizeof(T);
    impl_.elementSize = sizeof(T);
    impl_.out = std::move(bufs);
  }

  template <typename T>
  void setOutput(T* ptr, size_t elements) {
    setOutputs(&ptr, 1, elements);
  }

  template <typename T>
  void setOutputs(std::vector<T*> ptrs, size_t elements) {
    //printf("set outputs\n");
    // default is float
    impl_.scalarType = ScalarType::FLOAT;
    
#if GLOO_USE_TORCH_DTYPES
if (std::is_same_v<T, c10::Half>) {
    //GPF_PRINT("output type is half");
    impl_.scalarType = ScalarType::HALF;
} else if (std::is_same_v<T, c10::BFloat16>) {
    impl_.scalarType = ScalarType::BFLOAT16;
    //GPF_PRINT("output type is bfloat16");
}
#endif
    setOutputs(ptrs.data(), ptrs.size(), elements);
  }

  

  template <typename T>
  void setOutputs(T** ptrs, size_t len, size_t elements) {
    impl_.elements = elements;
    impl_.elementSize = sizeof(T);
    impl_.out.reserve(len);
    for (size_t i = 0; i < len; i++) {
      impl_.out.push_back(
          impl_.context->createUnboundBuffer(ptrs[i], elements * sizeof(T)));
    }
  }

  void setReduceFunction(Func fn) {
    impl_.reduce = fn;
  }

  void setTag(uint32_t tag) {
    impl_.tag = tag;
  }

  void setMaxSegmentSize(size_t maxSegmentSize) {
    impl_.maxSegmentSize = maxSegmentSize;
  }

  void setTimeout(std::chrono::milliseconds timeout) {
    impl_.timeout = timeout;
  }

 protected:
  detail::AllreduceOptionsImpl impl_;

  friend void allreduce(const AllreduceOptions&);
};

void allreduce(const AllreduceOptions& opts);

} // namespace gloo
