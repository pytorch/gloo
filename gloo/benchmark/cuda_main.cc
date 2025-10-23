/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <memory>

#include "gloo/benchmark/benchmark.h"
#include "gloo/benchmark/runner.h"
#include "gloo/common/enforce.h"
#include "gloo/cuda_allreduce_bcube.h"
#include "gloo/cuda_allreduce_halving_doubling.h"
#include "gloo/cuda_allreduce_halving_doubling_pipelined.h"
#include "gloo/cuda_allreduce_ring_chunked.h"
#include "gloo/cuda_broadcast_one_to_all.h"
#include "gloo/cuda_private.h"

using namespace gloo;
using namespace gloo::benchmark;

namespace {

int cudaNumDevices() {
  int n = 0;
  CUDA_CHECK(cudaGetDeviceCount(&n));
  return n;
}

template <typename T>
class CudaBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  ~CudaBenchmark() override {}

 protected:
  // Allocates memory for algorithm under benchmark to use. It calls
  // the allocate function on the superclass to get initialized host
  // side memory. This is then copied to device memory, instead of
  // duplicating initialization code here. The host side memory is
  // reused when the benchmark runs in verification mode and memory
  // contents is checked after an algorithm has run.
  std::vector<T*> allocate(int inputs, size_t elements) override {
    auto rawHostPtrs = Benchmark<T>::allocate(inputs, elements);

    // Initialize allocations, streams, CUDA pointer wrappers
    for (auto i = 0; i < inputs; i++) {
      auto device = i % cudaNumDevices();

      CudaDeviceScope scope(device);
      allocations_.push_back(CudaMemory<T>(elements));
      streams_.push_back(CudaStream(device));
      hostPtrs_.push_back(CudaHostPointer<T>::create(rawHostPtrs[i], elements));
      devicePtrs_.push_back(
          CudaDevicePointer<T>::create(*allocations_[i], elements));
    }

    // Copy initialized inputs to device
    for (auto i = 0; i < inputs; i++) {
      streams_[i].copyAsync(devicePtrs_[i], hostPtrs_[i]);
    }

    // Wait for copy to complete and populate return vector
    std::vector<T*> rawDevicePtrs;
    for (auto i = 0; i < inputs; i++) {
      streams_[i].wait();
      rawDevicePtrs.push_back(*devicePtrs_[i]);
    }

    return rawDevicePtrs;
  }

  std::vector<T*> copyToHost() {
    for (auto i = 0; i < hostPtrs_.size(); i++) {
      streams_[i].copyAsync(hostPtrs_[i], devicePtrs_[i]);
    }
    std::vector<T*> rawHostPtrs;
    for (auto i = 0; i < hostPtrs_.size(); i++) {
      streams_[i].wait();
      rawHostPtrs.push_back(*hostPtrs_[i]);
    }
    return rawHostPtrs;
  }

  std::vector<CudaMemory<T>> allocations_;
  std::vector<CudaStream> streams_;
  std::vector<CudaHostPointer<T>> hostPtrs_;
  std::vector<CudaDevicePointer<T>> devicePtrs_;
};

template <typename T, typename Algorithm>
class CudaAllreduceBenchmark : public CudaBenchmark<T> {
 public:
  CudaAllreduceBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : CudaBenchmark<T>(context, options) {}

  void initialize(size_t elements) override {
    auto ptrs = this->allocate(this->options_.inputs, elements);
    this->algorithm_ =
        gloo::make_unique<Algorithm>(this->context_, ptrs, elements);
  }

  void verify() override {
    // Size is the total number of pointers across the context
    const auto size = this->context_->size * this->inputs_.size();
    // Expected is set to the expected value at ptr[0]
    const auto expected = (size * (size - 1)) / 2;
    // The stride between values at subsequent indices is equal to
    // "size", and we have "size" of them. Therefore, after
    // allreduce, the stride between expected values is "size^2".
    const auto stride = size * size;
    const auto ptrs = this->copyToHost();
    const auto elements = this->hostPtrs_[0].getCount();
    for (const auto& ptr : ptrs) {
      for (int i = 0; i < elements; i++) {
        auto offset = i * stride;
        GLOO_ENFORCE_EQ(T(offset + expected), ptr[i], "Mismatch at index: ", i);
      }
    }
  }
};

template <typename T, typename Algorithm>
class CudaBroadcastBenchmark : public CudaBenchmark<T> {
  using CudaBenchmark<T>::CudaBenchmark;

 public:
  CudaBroadcastBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : CudaBenchmark<T>(context, options) {}

 public:
  void initialize(size_t elements) override {
    auto ptrs = this->allocate(this->options_.inputs, elements);
    this->algorithm_ = gloo::make_unique<Algorithm>(
        this->context_, ptrs, elements, rootRank_, rootPointerRank_);
  }

  void verify() override {
    const auto rootOffset = rootRank_ * this->inputs_.size() + rootPointerRank_;
    const auto stride = this->context_->size * this->inputs_.size();
    const auto ptrs = this->copyToHost();
    const auto elements = this->hostPtrs_[0].getCount();
    for (const auto& ptr : ptrs) {
      for (int i = 0; i < elements; i++) {
        auto offset = i * stride;
        GLOO_ENFORCE_EQ(
            T(rootOffset + offset), ptr[i], "Mismatch at index: ", i);
      }
    }
  }

 protected:
  const int rootRank_ = 0;
  const int rootPointerRank_ = 0;
};

} // namespace

template <class TC>
bool beginsWith(const TC& input, const TC& match) {
  return input.size() >= match.size() &&
      equal(match.begin(), match.end(), input.begin());
}

template <typename T>
void runBenchmark(options& x) {
  Runner::BenchmarkFn<T> fn;

  if (x.benchmark == "cuda_broadcast_one_to_all") {
    fn = [&x](std::shared_ptr<Context>& context) {
      return std::unique_ptr<Benchmark<T>>(
          new CudaBroadcastBenchmark<T, CudaBroadcastOneToAll<T>>(context, x));
    };
  } else if (x.benchmark == "cuda_allreduce_halving_doubling") {
    fn = [&x](std::shared_ptr<Context>& context) {
      return std::unique_ptr<Benchmark<T>>(
          new CudaAllreduceBenchmark<T, CudaAllreduceHalvingDoubling<T>>(
              context, x));
    };
  } else if (x.benchmark == "cuda_allreduce_halving_doubling_pipelined") {
    fn = [&x](std::shared_ptr<Context>& context) {
      return std::unique_ptr<Benchmark<T>>(
          new CudaAllreduceBenchmark<
              T,
              CudaAllreduceHalvingDoublingPipelined<T>>(context, x));
    };
  } else if (x.benchmark == "cuda_allreduce_bcube") {
    fn = [&x](std::shared_ptr<Context>& context) {
      return std::unique_ptr<Benchmark<T>>(
          new CudaAllreduceBenchmark<T, CudaAllreduceBcube<T>>(context, x));
    };
  } else if (x.benchmark == "cuda_allreduce_ring_chunked") {
    fn = [&x](std::shared_ptr<Context>& context) {
      return std::unique_ptr<Benchmark<T>>(
          new CudaAllreduceBenchmark<T, CudaAllreduceRingChunked<T>>(
              context, x));
    };
  }

  if (!fn) {
    GLOO_ENFORCE(false, "Invalid algorithm: ", x.benchmark);
  }

  Runner r(x);
  r.run(fn);
}

int main(int argc, char** argv) {
  auto x = benchmark::parseOptions(argc, argv);
  if (x.halfPrecision) {
    runBenchmark<float16>(x);
  } else {
    runBenchmark<float>(x);
  }
  return 0;
}
