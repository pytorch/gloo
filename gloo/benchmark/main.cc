/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <string>

#include "gloo/allgather.h"
#include "gloo/allgather_ring.h"
#include "gloo/allgatherv.h"
#include "gloo/allreduce.h"
#include "gloo/allreduce_bcube.h"
#include "gloo/allreduce_halving_doubling.h"
#include "gloo/allreduce_local.h"
#include "gloo/allreduce_ring.h"
#include "gloo/allreduce_ring_chunked.h"
#include "gloo/alltoall.h"
#include "gloo/alltoallv.h"
#include "gloo/barrier_all_to_all.h"
#include "gloo/barrier_all_to_one.h"
#include "gloo/broadcast.h"
#include "gloo/broadcast_one_to_all.h"
#include "gloo/common/aligned_allocator.h"
#include "gloo/common/common.h"
#include "gloo/common/enforce.h"
#include "gloo/context.h"
#include "gloo/pairwise_exchange.h"
#include "gloo/reduce.h"
#include "gloo/reduce_scatter.h"
#include "gloo/scatter.h"
#include "gloo/types.h"

#include "gloo/benchmark/benchmark.h"
#include "gloo/benchmark/runner.h"

using namespace gloo;
using namespace gloo::benchmark;

namespace {

// constant offset used for alltoall when populating input data
constexpr int kAlltoallOffset = 127;
// constant slot used for send/recv
constexpr uint64_t kSlot = 0x1337;
// exact number of processes needed for send/recv benchmarks
constexpr uint64_t kSendRecvProcesses = 2;

// constant strings for error messages
const std::string kMismatchErrorString = "Mismatch at index: ";
const std::string kNumProcessesErrorString =
    "Incorrect number of processes used for send/recv benchmarks (please use 2 processes): ";

// Returns the rank as a string with format "Rank: rank "
std::string formatRank(int rank) {
  return "Rank: " + std::to_string(rank) + " ";
}

// Verify function used for AllgatherBenchmark and
// AllgatherRingBenchmark. The result/output from both
// should be the same, but created two separate classes because
// the setup is different for each implementation of the collective
template <typename T>
void allgatherVerify(
    std::vector<T> outputs,
    int size,
    int inputs,
    int elements,
    std::vector<std::string>& errors) {
  // Stride is the total number of total number of
  // pointers across the context
  const auto stride = size * inputs;
  for (int rank = 0; rank < size; rank++) {
    auto val = rank * inputs;
    for (int elem = 0; elem < elements; elem++) {
      T expected(elem * stride + val);
      for (int input = 0; input < inputs; input++) {
        const auto rankOffset = rank * elements * inputs;
        const auto inputOffset = input * elements;
        try {
          GLOO_ENFORCE_EQ(
              outputs[rankOffset + inputOffset + elem],
              expected + T(input),
              kMismatchErrorString,
              "[",
              rank,
              ", ",
              input,
              ", ",
              elem,
              "]");
        } catch (::gloo::EnforceNotMet& e) {
          errors.push_back(formatRank(rank) + e.msg());
        }
      }
    }
  }
}

// Many of the benchmarks result in a constant
// stride between each value in the array. This helper
// function allows you to specify the base / stride and
// verifies if the pattern exists.
// e.g. If you expect input to be [1, 3, 5, 7]
//      use base = 1 and stride = 2
template <typename T>
void constStrideVerify(
    std::vector<std::vector<T, aligned_allocator<T, kBufferAlignment>>>& inputs,
    int base,
    int stride,
    int rank,
    std::vector<std::string>& errors) {
  for (const auto& input : inputs) {
    for (int i = 0; i < input.size(); i++) {
      auto offset = i * stride;
      try {
        GLOO_ENFORCE_EQ(
            // Offset changes by a constant stride each iteration
            T(offset + base),
            input[i],
            kMismatchErrorString,
            i);
      } catch (::gloo::EnforceNotMet& e) {
        errors.push_back(formatRank(rank) + e.msg());
      }
    }
  }
}

template <typename T>
class AllgatherBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  AllgatherBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Create input/output buffers
    auto inPtrs = this->allocate(this->options_.inputs, elements);
    output_.resize(this->options_.inputs * this->context_->size * elements);

    // Configure AllgatherOptions struct
    opts_.setInput(inPtrs.front(), elements);
    opts_.setOutput(output_.data(), this->context_->size * elements);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    allgather(opts_);
  }

  // Verify is identical for AllgatherBenchmark
  // and AllgatherRingBenchmark
  void verify(std::vector<std::string>& errors) override {
    allgatherVerify(
        output_,
        this->context_->size,
        this->inputs_.size(),
        this->inputs_[0].size(),
        errors);
  }

 protected:
  AllgatherOptions opts_;

  // Used to configure options
  std::vector<T> output_;
};

template <typename T>
class AllgathervBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  AllgathervBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Initialize input/output buffers
    auto size = this->context_->size;
    auto inPtrs = this->allocate(this->options_.inputs, elements * size);
    output_.resize(elements * (size * (size - 1)) / 2);

    // Initialize counts
    counts_.resize(size);
    GLOO_ENFORCE(
        counts_.size() == size,
        "Size mismatch for counts in AllgathervBenchmark");
    for (auto i = 0; i < size; i++) {
      counts_[i] = i * elements;
    }

    // Configure AllgathervOptions struct
    opts_.setInput<T>(inPtrs.front(), this->context_->rank * elements);
    opts_.setOutput<T>(output_.data(), counts_);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    allgatherv(opts_);
  }

  void verify(std::vector<std::string>& errors) override {
    const int size = this->context_->size;
    const auto stride = size * this->options_.inputs;
    size_t offset = 0;
    for (auto i = 0; i < size; i++) {
      for (auto j = 0; j < counts_[i]; j++) {
        try {
          GLOO_ENFORCE_EQ(
              T(j * stride + i),
              output_[offset + j],
              kMismatchErrorString,
              offset + j);
        } catch (::gloo::EnforceNotMet& e) {
          errors.push_back(formatRank(this->context_->rank) + e.msg());
        }
      }
      offset += counts_[i];
    }
  }

 protected:
  AllgathervOptions opts_;

  // Used to configure options
  std::vector<T> output_;
  std::vector<size_t> counts_;
};

template <typename T>
class AllgatherRingBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t elements) override {
    auto inPtrs = this->allocate(this->options_.inputs, elements);
    GLOO_ENFORCE_EQ(inPtrs.size(), this->options_.inputs);
    outputs_.resize(this->options_.inputs * this->context_->size * elements);
    this->algorithm_.reset(new AllgatherRing<T>(
        this->context_, this->getInputs(), outputs_.data(), elements));
  }

  // Verify is identical for AllgatherBenchmark
  // and AllgatherRingBenchmark
  void verify(std::vector<std::string>& errors) override {
    allgatherVerify(
        outputs_,
        this->context_->size,
        this->inputs_.size(),
        this->inputs_[0].size(),
        errors);
  }

 protected:
  std::vector<T> outputs_;
};

template <class A, typename T>
class AllreduceBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t elements) override {
    auto ptrs = this->allocate(this->options_.inputs, elements);
    this->algorithm_.reset(new A(this->context_, ptrs, elements));
  }

  void verify(std::vector<std::string>& errors) override {
    // Size is the total number of pointers across the context
    const auto size = this->context_->size * this->inputs_.size();

    // allreduce_local does not have knowledge of the other
    // processes. So, it essentially reduces on a single
    // process meaning that the output should be identical
    // to the input.
    if (this->options_.benchmark == "allreduce_local") {
      // Stride is equal to the "size" since we only have one process
      const auto stride = size;
      // Expected value at ptr[0] should just be
      // the rank since the input size is 1
      const auto expected = this->context_->rank;

      constStrideVerify(
          this->inputs_, expected, stride, this->context_->rank, errors);
      return;
    }

    // For all other allreduce algorithms:
    // Expected is set to the expected value at ptr[0]
    const auto expected = (size * (size - 1)) / 2;
    // The stride between values at subsequent indices is equal to
    // "size", and we have "size" of them. Therefore, after
    // allreduce, the stride between expected values is "size^2".
    const auto stride = size * size;
    constStrideVerify(
        this->inputs_, expected, stride, this->context_->rank, errors);
  }
};

template <typename T>
class AllToAllBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  AllToAllBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Create new input/output vectors based on number of elements
    int size = this->context_->size;
    input_ = std::vector<uint64_t>(size * elements);
    output_ = std::vector<uint64_t>(size * elements);

    // Populate data for the input
    for (int i = 0; i < size; i++) {
      for (int j = 0; j < elements; j++) {
        input_[i * elements + j] =
            this->context_->rank * j + i * kAlltoallOffset;
      }
    }

    // Configure AlltoallOptions struct
    opts_.setInput(input_.data(), size * elements);
    opts_.setOutput(output_.data(), size * elements);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    alltoall(opts_);
  }

  void verify(std::vector<std::string>& errors) override {
    const int rank = this->context_->rank;
    for (const auto& input : this->inputs_) {
      const int size = input.size();
      for (int i = 0; i < size; i++) {
        try {
          GLOO_ENFORCE_EQ(
              output_[rank * size + i],
              rank * (kAlltoallOffset + i),
              kMismatchErrorString,
              rank * size + i);
        } catch (::gloo::EnforceNotMet& e) {
          errors.push_back(formatRank(rank) + e.msg());
        }
      }
    }
  }

 protected:
  AlltoallOptions opts_;

  // input and output vectors used to configure options
  std::vector<uint64_t> input_;
  std::vector<uint64_t> output_;
};

template <typename T>
class AllToAllvBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  AllToAllvBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Get size and rank
    int size = this->context_->size;
    int rank = this->context_->rank;

    // Calculate input/output length
    size_t inLength = size * (rank + 1) + size * (size - 1) / 2;
    size_t outlength = size * (size - rank) + size * (size - 1) / 2;

    // Initialize input and output
    input_ = std::vector<uint64_t>(inLength * elements);
    output_ = std::vector<uint64_t>(outlength * elements);

    // Fill input buffer
    size_t offset = 0;
    for (int i = 0; i < size; i++) {
      size_t length = size + rank - i;
      for (int j = 0; j < length * elements; j++) {
        input_[offset + j] = rank * j + i * kAlltoallOffset;
      }
      offset += length * elements;
    }

    // Set up splits
    for (int i = 0; i < size; i++) {
      inElementsPerRank_.push_back(elements * (rank + size - i));
      outElementsPerRank_.push_back(elements * (size - rank + i));
    }

    // Configure AlltoallvOptions struct
    opts_.setInput(input_.data(), inElementsPerRank_);
    opts_.setOutput(output_.data(), outElementsPerRank_);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    alltoallv(opts_);
  }

  void verify(std::vector<std::string>& errors) override {
    const int size = this->context_->size;
    const int rank = this->context_->rank;
    for (const auto& input : this->inputs_) {
      int dataSize = input.size();
      for (int i = 0; i < size * dataSize; i++) {
        try {
          GLOO_ENFORCE_EQ(
              output_[i],
              rank * (kAlltoallOffset + i),
              kMismatchErrorString,
              i);
        } catch (::gloo::EnforceNotMet& e) {
          errors.push_back(formatRank(rank) + e.msg());
        }
      }
    }
  }

 protected:
  AlltoallvOptions opts_;

  // input and output vectors used to configure options
  std::vector<uint64_t> input_;
  std::vector<uint64_t> output_;
  // split vectors used to configure options
  std::vector<int64_t> inElementsPerRank_;
  std::vector<int64_t> outElementsPerRank_;
};

template <typename T>
class BarrierAllToAllBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t /* unused */) override {
    this->algorithm_.reset(new BarrierAllToAll(this->context_));
  }
};

template <typename T>
class BarrierAllToOneBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t /* unused */) override {
    // This tool measures at rank=0, so use root=1 for the all to one
    // barrier to measure the end-to-end latency (otherwise we might
    // not account for the send-to-root part of the algorithm).
    this->algorithm_.reset(new BarrierAllToOne(this->context_, 1));
  }
};

template <typename T>
class BroadcastBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  BroadcastBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Create input buffer
    auto inPtrs = this->allocate(this->options_.inputs, elements);
    // Configure BroadcastOptions struct
    // Use rank 0 as root
    opts_.setRoot(rootRank_);
    // Do in place, use input as output
    opts_.setOutput(inPtrs.front(), elements);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    broadcast(opts_);
  }

  void verify(std::vector<std::string>& errors) override {
    // Stride is the total number of
    // pointers across the context
    auto stride = this->context_->size * this->inputs_.size();
    constStrideVerify(
        this->inputs_, rootRank_, stride, this->context_->rank, errors);
  }

 protected:
  BroadcastOptions opts_;

  // Always use rank 0 as the root
  const int rootRank_ = 0;
};

template <typename T>
class BroadcastOneToAllBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t elements) override {
    auto ptrs = this->allocate(this->options_.inputs, elements);
    this->algorithm_.reset(
        new BroadcastOneToAll<T>(this->context_, ptrs, elements, rootRank_));
  }

  void verify(std::vector<std::string>& errors) override {
    const auto stride = this->context_->size * this->inputs_.size();
    constStrideVerify(
        this->inputs_, rootRank_, stride, this->context_->rank, errors);
  }

 protected:
  const int rootRank_ = 0;
};

template <typename T>
class PairwiseExchangeBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t elements) override {
    this->algorithm_.reset(new PairwiseExchange(
        this->context_, elements, this->getOptions().destinations));
  }
};

template <typename T>
class ReduceBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  ReduceBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Create input/output buffers
    auto inPtrs = this->allocate(this->options_.inputs, elements);
    output_.resize(elements);

    // Configure ReduceOptions struct
    // Use rank 0 as root
    opts_.setRoot(rootRank_);
    // Set reduce function
    void (*fn)(void*, const void*, const void*, long unsigned int) = &sum<T>;
    opts_.setReduceFunction(fn);
    // MaxSegmentSize must be a multiple of the element size T
    // Can't be too small otherwise benchmark will run for a long time
    // Use a factor of (elements / 2)
    opts_.setMaxSegmentSize(sizeof(T) * elements / 2);
    opts_.setInput(inPtrs.front(), elements);
    opts_.setOutput(output_.data(), elements);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    reduce(opts_);
  }

  void verify(std::vector<std::string>& errors) override {
    // Size is the total number of pointers across the context
    const auto size = this->context_->size * this->inputs_.size();
    // Expected is set to be the expected value of ptr[0]
    // after reduce gets called (calculation depends on the
    // reduction function used and how we initialized the inputs)
    const auto expected = (size * (size - 1)) / 2;
    // The stride between values at subsequent indices is equal to
    // "size", and we have "size" of them. Therefore, after
    // reduce, the stride between expected values is "size^2".
    const auto stride = size * size;

    // Verify only for root
    if (this->context_->rank == rootRank_) {
      for (int i = 0; i < output_.size(); i++) {
        auto offset = i * stride;
        try {
          GLOO_ENFORCE_EQ(
              T(offset + expected), output_[i], kMismatchErrorString, i);
        } catch (::gloo::EnforceNotMet& e) {
          errors.push_back(formatRank(rootRank_) + e.msg());
        }
      }
    }
  }

 protected:
  ReduceOptions opts_;

  // Always use rank 0 as the root
  const int rootRank_ = 0;
  std::vector<T> output_;
};

template <typename T>
class ReduceScatterBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t elements) override {
    auto ptrs = this->allocate(this->options_.inputs, elements);
    auto rem = elements;
    auto chunkSize =
        (elements + this->context_->size - 1) / this->context_->size;
    for (int i = 0; i < this->context_->size; ++i) {
      recvCounts_.push_back(std::min(chunkSize, rem));
      rem = rem > chunkSize ? rem - chunkSize : 0;
    }
    this->algorithm_.reset(new ReduceScatterHalvingDoubling<T>(
        this->context_, ptrs, elements, recvCounts_));
  }

  void verify(std::vector<std::string>& errors) override {
    // Size is the total number of pointers across the context
    const auto size = this->context_->size * this->inputs_.size();
    // Expected is set to the expected value at ptr[0]
    const auto expected = (size * (size - 1)) / 2;
    // The stride between values at subsequent indices is equal to
    // "size", and we have "size" of them. Therefore, after
    // reduce-scatter, the stride between expected values is "size^2".
    const auto stride = size * size;
    for (const auto& input : this->inputs_) {
      int numElemsSoFar = 0;
      for (int i = 0; i < this->context_->rank; ++i) {
        numElemsSoFar += recvCounts_[i];
      }
      for (int i = 0; i < recvCounts_[this->context_->rank]; ++i) {
        auto offset = (numElemsSoFar + i) * stride;
        try {
          GLOO_ENFORCE_EQ(
              T(offset + expected), input[i], kMismatchErrorString, i);
        } catch (::gloo::EnforceNotMet& e) {
          errors.push_back(formatRank(this->context_->rank) + e.msg());
        }
      }
    }
  }

 protected:
  std::vector<int> recvCounts_;
};

template <typename T>
class ScatterBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  ScatterBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  void initialize(size_t elements) override {
    // Create input buffer
    auto inPtrs = this->allocate(this->context_->size, elements);
    output_.resize(elements);

    // Configure ReduceOptions struct
    // Use rank 0 as root
    opts_.setRoot(rootRank_);
    opts_.setInputs(inPtrs, elements);
    opts_.setOutput(output_.data(), elements);
  }

  // Default run function calls Algorithm::run
  // Need to override this function for collectives that
  // do not inherit from the Algorithm class
  void run() override {
    // Run the collective on the previously created options
    scatter(opts_);
  }

  void verify(std::vector<std::string>& errors) override {
    auto stride = this->context_->size * this->inputs_.size();
    for (int i = 0; i < output_.size(); i++) {
      const auto base =
          (rootRank_ * this->context_->size) + this->context_->rank;
      const auto offset = i * stride;
      try {
        GLOO_ENFORCE_EQ(T(base + offset), output_[i], kMismatchErrorString, i);
      } catch (::gloo::EnforceNotMet& e) {
        errors.push_back(formatRank(this->context_->rank) + e.msg());
      }
    }
  }

 protected:
  ScatterOptions opts_;

  // Always use rank 0 as the root
  const int rootRank_ = 0;
  std::vector<T> output_;
};

template <typename T>
class SendRecvRoundtripBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  void initialize(size_t elements) override {
    auto ptr = this->allocate(this->options_.inputs, elements);
    buf_ =
        this->context_->createUnboundBuffer(ptr.front(), elements * sizeof(T));
  }

  void run() override {
    if (this->context_->rank == source_) {
      const int other = 1;
      // If source rank, send first
      buf_->send(other, kSlot);
      buf_->waitSend();
      // and receive after
      buf_->recv(other, kSlot);
      buf_->waitRecv();
    } else {
      // Otherwise, receive from source first
      buf_->recv(source_, kSlot);
      buf_->waitRecv();
      // and send after
      buf_->send(source_, kSlot);
      buf_->waitSend();
    }
  }

  void verify(std::vector<std::string>& errors) override {
    // Stride is the total number of
    // pointers across the context
    auto stride = this->context_->size * this->inputs_.size();
    constStrideVerify(
        this->inputs_, source_, stride, this->context_->rank, errors);
  }

 protected:
  std::unique_ptr<transport::UnboundBuffer> buf_;
  // Data will always be sent from rank 0 to rank 1 and
  // then back to rank 0, so the source rank will always be 0
  const int source_ = 0;
};

// This benchmark shows the time it takes to send
// a large number (default 10000, can be set from command line)
// of messages of size elements from point A->B.
// Can be run synchronously or asynchronously.
template <typename T>
class SendRecvStressBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

 public:
  SendRecvStressBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options,
      bool async)
      : Benchmark<T>(context, options), async_(async) {}

  void initialize(size_t elements) override {
    auto ptr = this->allocate(this->options_.inputs, elements);
    buf_ =
        this->context_->createUnboundBuffer(ptr.front(), elements * sizeof(T));
  }

  void run() override {
    const int niters = this->options_.messages;
    // Only send on process with rank 0
    if (this->context_->rank == srcRank_) {
      for (int i = 0; i < niters; i++) {
        buf_->send(dstRank_, kSlot);
        // If we run synchronously, call waitSend after each send
        if (!async_) {
          buf_->waitSend();
        }
      }
      // If we run asynchronously, call waitSend after all sends
      if (async_) {
        for (int i = 0; i < niters; i++) {
          buf_->waitSend();
        }
      }
      // Only recv on process with rank 1
    } else {
      for (int i = 0; i < niters; i++) {
        buf_->recv(srcRank_, kSlot);
        // If we run synchronously, call waitRecv after each recv
        if (!async_) {
          buf_->waitRecv();
        }
      }
      // If we run asynchronously, call waitRecv after all recvs
      if (async_) {
        for (int i = 0; i < niters; i++) {
          buf_->waitRecv();
        }
      }
    }
  }

  void verify(std::vector<std::string>& errors) override {
    // Only verify for rank that actually got sent data
    if (this->context_->rank == dstRank_) {
      // Stride is the total number of
      // pointers across the context
      auto stride = this->context_->size * this->inputs_.size();
      constStrideVerify(
          this->inputs_, srcRank_, stride, this->context_->rank, errors);
    }
  }

 protected:
  std::unique_ptr<transport::UnboundBuffer> buf_;
  // Always send from rank 0 and receive from rank 1
  const int srcRank_ = 0;
  const int dstRank_ = 1;
  // Whether to send/recv asynchronously or not
  const bool async_;
};

} // namespace

// Namespace for the new style algorithm benchmarks.
namespace {

template <typename T>
class NewAllreduceBenchmark : public Benchmark<T> {
  using allocation =
      std::vector<std::vector<T, aligned_allocator<T, kBufferAlignment>>>;

 public:
  NewAllreduceBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), opts_(context) {}

  allocation newAllocation(int inputs, size_t elements) {
    allocation out;
    out.reserve(inputs);
    for (size_t i = 0; i < inputs; i++) {
      out.emplace_back(elements);
    }
    return out;
  }

  void initialize(size_t elements) override {
    inputAllocation_ = newAllocation(this->options_.inputs, elements);
    outputAllocation_ = newAllocation(this->options_.inputs, elements);

    // Stride between successive values in any input.
    const auto stride = this->context_->size * this->options_.inputs;
    for (size_t i = 0; i < this->options_.inputs; i++) {
      // Different for every input at every node. This means all
      // values across all inputs and all nodes are different and we
      // can accurately detect correctness errors.
      const auto value = (this->context_->rank * this->options_.inputs) + i;
      for (size_t j = 0; j < elements; j++) {
        inputAllocation_[i][j] = (j * stride) + value;
      }
    }

    // Generate vectors with pointers to populate the options struct.
    std::vector<T*> inputPointers;
    std::vector<T*> outputPointers;
    for (size_t i = 0; i < this->options_.inputs; i++) {
      inputPointers.push_back(inputAllocation_[i].data());
      outputPointers.push_back(outputAllocation_[i].data());
    }

    // Configure AllreduceOptions struct
    opts_.setInputs(inputPointers, elements);
    opts_.setOutputs(outputPointers, elements);
    opts_.setAlgorithm(AllreduceOptions::Algorithm::RING);
    void (*fn)(void*, const void*, const void*, long unsigned int) = &sum<T>;
    opts_.setReduceFunction(fn);
  }

  void run() override {
    allreduce(opts_);
  }

 private:
  AllreduceOptions opts_;

  allocation inputAllocation_;
  allocation outputAllocation_;
};

} // namespace

#define RUN_BENCHMARK(T)                                                       \
  Runner::BenchmarkFn<T> fn;                                                   \
  if (x.benchmark == "allgather") {                                            \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllgatherBenchmark<T>>(context, x);             \
    };                                                                         \
  } else if (x.benchmark == "allgather_v") {                                   \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllgathervBenchmark<T>>(context, x);            \
    };                                                                         \
  } else if (x.benchmark == "allgather_ring") {                                \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllgatherRingBenchmark<T>>(context, x);         \
    };                                                                         \
  } else if (x.benchmark == "allreduce_ring") {                                \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllreduceBenchmark<AllreduceRing<T>, T>>(       \
          context, x);                                                         \
    };                                                                         \
  } else if (x.benchmark == "allreduce_ring_chunked") {                        \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<                                                \
          AllreduceBenchmark<AllreduceRingChunked<T>, T>>(context, x);         \
    };                                                                         \
  } else if (x.benchmark == "allreduce_halving_doubling") {                    \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<                                                \
          AllreduceBenchmark<AllreduceHalvingDoubling<T>, T>>(context, x);     \
    };                                                                         \
  } else if (x.benchmark == "allreduce_bcube") {                               \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllreduceBenchmark<AllreduceBcube<T>, T>>(      \
          context, x);                                                         \
    };                                                                         \
  } else if (x.benchmark == "allreduce_local") {                               \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllreduceBenchmark<AllreduceLocal<T>, T>>(      \
          context, x);                                                         \
    };                                                                         \
  } else if (x.benchmark == "alltoall") {                                      \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllToAllBenchmark<T>>(context, x);              \
    };                                                                         \
  } else if (x.benchmark == "alltoall_v") {                                    \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<AllToAllvBenchmark<T>>(context, x);             \
    };                                                                         \
  } else if (x.benchmark == "barrier_all_to_all") {                            \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<BarrierAllToAllBenchmark<T>>(context, x);       \
    };                                                                         \
  } else if (x.benchmark == "barrier_all_to_one") {                            \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<BarrierAllToOneBenchmark<T>>(context, x);       \
    };                                                                         \
  } else if (x.benchmark == "broadcast") {                                     \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<BroadcastBenchmark<T>>(context, x);             \
    };                                                                         \
  } else if (x.benchmark == "broadcast_one_to_all") {                          \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<BroadcastOneToAllBenchmark<T>>(context, x);     \
    };                                                                         \
  } else if (x.benchmark == "pairwise_exchange") {                             \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<PairwiseExchangeBenchmark<T>>(context, x);      \
    };                                                                         \
  } else if (x.benchmark == "reduce") {                                        \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<ReduceBenchmark<T>>(context, x);                \
    };                                                                         \
  } else if (x.benchmark == "reduce_scatter") {                                \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<ReduceScatterBenchmark<T>>(context, x);         \
    };                                                                         \
  } else if (x.benchmark == "scatter") {                                       \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<ScatterBenchmark<T>>(context, x);               \
    };                                                                         \
  } else if (x.benchmark == "sendrecv_roundtrip") {                            \
    GLOO_ENFORCE_EQ(                                                           \
        x.contextSize,                                                         \
        kSendRecvProcesses,                                                    \
        kNumProcessesErrorString,                                              \
        x.contextSize);                                                        \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<SendRecvRoundtripBenchmark<T>>(context, x);     \
    };                                                                         \
  } else if (x.benchmark == "sendrecv_stress") {                               \
    GLOO_ENFORCE_EQ(                                                           \
        x.contextSize,                                                         \
        kSendRecvProcesses,                                                    \
        kNumProcessesErrorString,                                              \
        x.contextSize);                                                        \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<SendRecvStressBenchmark<T>>(context, x, false); \
    };                                                                         \
  } else if (x.benchmark == "isendirecv_stress") {                             \
    GLOO_ENFORCE_EQ(                                                           \
        x.contextSize,                                                         \
        kSendRecvProcesses,                                                    \
        kNumProcessesErrorString,                                              \
        x.contextSize);                                                        \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<SendRecvStressBenchmark<T>>(context, x, true);  \
    };                                                                         \
  }                                                                            \
  if (!fn) {                                                                   \
    GLOO_ENFORCE(false, "Invalid algorithm: ", x.benchmark);                   \
  }                                                                            \
  Runner r(x);                                                                 \
  r.run(fn);

template <typename T>
void runNewBenchmark(options& options) {
  Runner::BenchmarkFn<T> fn;

  const auto name = options.benchmark.substr(4);
  if (name == "allreduce_ring") {
    fn = [&](std::shared_ptr<Context>& context) {
      return gloo::make_unique<NewAllreduceBenchmark<T>>(context, options);
    };
  } else {
    GLOO_ENFORCE(false, "Invalid benchmark name: ", options.benchmark);
  }

  Runner runner(options);
  runner.run(fn);
}

int main(int argc, char** argv) {
  auto x = benchmark::parseOptions(argc, argv);

  // Run new style benchmarks if the benchmark name starts with "new_".
  // Eventually we'd like to deprecate all the old style ones...
  if (x.benchmark.substr(0, 4) == "new_") {
    runNewBenchmark<float>(x);
    return 0;
  }

  if (x.benchmark == "pairwise_exchange") {
    RUN_BENCHMARK(char);
  } else if (x.halfPrecision) {
    RUN_BENCHMARK(float16);
  } else {
    RUN_BENCHMARK(float);
  }
  return 0;
}
