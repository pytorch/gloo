/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <sstream>
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
#include "gloo/barrier.h"
#include "gloo/barrier_all_to_all.h"
#include "gloo/barrier_all_to_one.h"
#include "gloo/broadcast.h"
#include "gloo/broadcast_one_to_all.h"
#include "gloo/common/aligned_allocator.h"
#include "gloo/common/common.h"
#include "gloo/common/logging.h"
#include "gloo/context.h"
#include "gloo/pairwise_exchange.h"
#include "gloo/reduce.h"
#include "gloo/reduce_scatter.h"
#include "gloo/scatter.h"
#include "gloo/types.h"

#include "gloo/benchmark/benchmark.h"
#include "gloo/benchmark/runner.h"

#include "gloo/transport/peel/peel_allgather.h"
#include "gloo/transport/peel/peel_allreduce_ring.h"
#include "gloo/transport/peel/peel_context.h"
#include "gloo/transport/peel/peel_discovery.h"

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
class BroadcastRingBenchmark : public BroadcastBenchmark<T> {
 public:
  BroadcastRingBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : BroadcastBenchmark<T>(context, options), barrierOpts_(context) {
    barrierOpts_.setTag(0xBADC0DE1);
  }

  void run() override {
    broadcast_ring(this->opts_);
    barrier(barrierOpts_);
  }

 protected:
  BarrierOptions barrierOpts_;
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

template <typename T>
class PeelBroadcastBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

  static std::shared_ptr<transport::peel::PeelContext> sharedCtx_;
  static std::mutex initMutex_;

 public:
  void initialize(size_t elements) override {
    GLOO_ENFORCE(
        !this->options_.peelIface.empty(),
        "peel_broadcast requires --peel-iface");
    GLOO_ENFORCE(
        !this->options_.peelTopologyFile.empty(),
        "peel_broadcast requires --peel-topology-file");
    GLOO_ENFORCE(
        this->options_.threads == 1,
        "peel_broadcast does not support --threads > 1 "
        "(shared PeelContext is not safe for concurrent broadcasts)");
    GLOO_ENFORCE(
        this->options_.iterationCount > 0,
        "peel_broadcast requires --iteration-count N "
        "(auto iteration scaling uses gloo TCP broadcast which times out "
        "with asymmetric subtrees)");

    // Use allocate() — same as BroadcastBenchmark. Fills this->inputs_[0]
    // with the stride pattern: inputs_[0][i] = i*stride + rank.
    // After broadcast from peelSenderRank, every rank must hold the sender's
    // pattern (i*stride + senderRank), so a silent no-op is detectable on
    // every rank at every index.
    this->allocate(this->options_.inputs, elements);

    std::lock_guard<std::mutex> lock(initMutex_);
    if (sharedCtx_) return;

    transport::peel::PeelDiscoveryConfig dc;
    dc.rank         = this->context_->rank;
    dc.world_size   = this->context_->size;
    dc.redis_host   = this->options_.redisHost;
    dc.redis_port   = this->options_.redisPort;
    dc.redis_prefix = this->options_.prefix + "/peel_ip";
    dc.iface_name   = this->options_.peelIface;
    dc.timeout_ms   = 300000;

    transport::peel::PeelDiscovery discovery(dc);
    GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

    transport::peel::PeelContextConfig cfg;
    cfg.rank          = this->context_->rank;
    cfg.world_size    = this->context_->size;
    cfg.peer_ips      = discovery.peerIps();
    cfg.mcast_group   = this->options_.peelMcastGroup;
    cfg.base_port     = static_cast<uint16_t>(this->options_.peelBasePort);
    cfg.iface_name    = this->options_.peelIface;
    cfg.ttl           = this->options_.peelTTL;
    cfg.sender_rank   = this->options_.peelSenderRank;
    cfg.topology_file = this->options_.peelTopologyFile;
    cfg.rto_ms        = this->options_.peelRtoMs;
    cfg.max_chunk_size = static_cast<size_t>(this->options_.peelMaxPayload);

    sharedCtx_ = std::make_shared<transport::peel::PeelContext>(cfg);
    GLOO_ENFORCE(sharedCtx_->init(), "PeelContext init failed");
  }

  void run() override {
    sharedCtx_->broadcast(
        this->options_.peelSenderRank,
        this->inputs_[0].data(),
        this->inputs_[0].size() * sizeof(T));
  }

  void verify(std::vector<std::string>& errors) override {
    // Identical to BroadcastBenchmark::verify — stride pattern rooted at
    // peelSenderRank. constStrideVerify checks inputs_[0][i] == i*stride + base.
    const auto stride = this->context_->size * this->inputs_.size();
    constStrideVerify(
        this->inputs_,
        this->options_.peelSenderRank,
        stride,
        this->context_->rank,
        errors);
  }
};

template <typename T>
std::shared_ptr<transport::peel::PeelContext>
    PeelBroadcastBenchmark<T>::sharedCtx_;

template <typename T>
std::mutex PeelBroadcastBenchmark<T>::initMutex_;

template <typename T>
class PeelBroadcastRingBenchmark : public Benchmark<T> {
  static std::shared_ptr<transport::peel::PeelContext> sharedCtx_;
  static std::mutex initMutex_;

 public:
  PeelBroadcastRingBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), barrierOpts_(context) {
    barrierOpts_.setTag(0xBADC0DE2);
  }

  void initialize(size_t elements) override {
    GLOO_ENFORCE(
        !this->options_.peelIface.empty(),
        "peel_broadcast_ring requires --peel-iface");
    GLOO_ENFORCE(
        this->options_.threads == 1,
        "peel_broadcast_ring does not support --threads > 1 "
        "(shared PeelContext is not safe for concurrent broadcasts)");
    GLOO_ENFORCE(
        this->options_.iterationCount > 0,
        "peel_broadcast_ring requires --iteration-count N "
        "(auto iteration scaling uses gloo TCP broadcast which times out "
        "with asymmetric subtrees)");

    this->allocate(this->options_.inputs, elements);

    std::lock_guard<std::mutex> lock(initMutex_);
    if (sharedCtx_) {
      return;
    }

    transport::peel::PeelDiscoveryConfig dc;
    dc.rank         = this->context_->rank;
    dc.world_size   = this->context_->size;
    dc.redis_host   = this->options_.redisHost;
    dc.redis_port   = this->options_.redisPort;
    dc.redis_prefix = this->options_.prefix + "/peel_ring_ip";
    dc.iface_name   = this->options_.peelIface;
    dc.timeout_ms   = 300000;

    transport::peel::PeelDiscovery discovery(dc);
    GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

    transport::peel::PeelContextConfig cfg;
    cfg.rank          = this->context_->rank;
    cfg.world_size    = this->context_->size;
    cfg.peer_ips      = discovery.peerIps();
    cfg.mcast_group   = this->options_.peelMcastGroup;
    cfg.base_port     = static_cast<uint16_t>(this->options_.peelBasePort);
    cfg.iface_name    = this->options_.peelIface;
    cfg.ttl           = this->options_.peelTTL;
    cfg.sender_rank   = this->options_.peelSenderRank;
    cfg.topology_file = this->options_.peelTopologyFile;
    cfg.rto_ms        = this->options_.peelRtoMs;
    cfg.max_chunk_size = static_cast<size_t>(this->options_.peelMaxPayload);

    sharedCtx_ = std::make_shared<transport::peel::PeelContext>(cfg);
    GLOO_ENFORCE(sharedCtx_->initRing(), "PeelContext ring init failed");
  }

  void run() override {
    GLOO_ENFORCE(
        sharedCtx_->broadcastRing(
            this->options_.peelSenderRank,
            this->inputs_[0].data(),
            this->inputs_[0].size() * sizeof(T)),
        "Peel ring broadcast failed");
    barrier(barrierOpts_);
  }

  void verify(std::vector<std::string>& errors) override {
    const auto stride = this->context_->size * this->inputs_.size();
    constStrideVerify(
        this->inputs_,
        this->options_.peelSenderRank,
        stride,
        this->context_->rank,
        errors);
  }

 protected:
  BarrierOptions barrierOpts_;
};

// =============================================================================
// PeelAllgatherBenchmark
//
// Runs world_size sequential or parallel peel broadcasts — one per sender rank
// — so that after each run() every rank holds every other rank's data.
//
// Buffer layout:
//   bufPtrs_[rank]  → inputs_[0].data()  (aligned, stride-filled via allocate)
//   bufPtrs_[r≠rank] → recvBufs_[r].data() (zeroed, overwritten by allgather)
//
// Send buffer pattern (from allocate(1, elements)):
//   inputs_[0][i] = i * worldSize + rank
//
// After allgather every bufPtrs_[r][i] must equal i * worldSize + r.
// This makes every element unique across all ranks and all indices, so
// element-level corruption is always detected.
// =============================================================================
template <typename T>
class PeelAllgatherBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

  // Shared across the benchmark lifetime (threads=1 enforced).
  static std::shared_ptr<transport::peel::PeelAllgather>            sharedAllgather_;
  static std::vector<std::shared_ptr<transport::peel::PeelContext>> sharedCtxs_;
  static std::mutex                                                       initMutex_;

  // Per-instance buffers reset on each initialize() call.
  std::vector<std::vector<T>> recvBufs_; // one zeroed buffer per r != rank
  std::vector<void*>          bufPtrs_;  // [rank]=inputs_[0], [r≠rank]=recvBufs_[r]
  size_t                      dataBytes_ = 0;

 public:
  void initialize(size_t elements) override {
    GLOO_ENFORCE(
        !this->options_.peelIface.empty(),
        "peel_allgather requires --peel-iface");
    GLOO_ENFORCE(
        !this->options_.peelTopologyFile.empty(),
        "peel_allgather requires --peel-topology-file");
    GLOO_ENFORCE(
        this->options_.threads == 1,
        "peel_allgather does not support --threads > 1");
    GLOO_ENFORCE(
        this->options_.iterationCount > 0,
        "peel_allgather requires --iteration-count N "
        "(auto iteration scaling uses gloo TCP broadcast which times out "
        "with asymmetric subtrees)");

    const int rank      = this->context_->rank;
    const int worldSize = this->context_->size;
    dataBytes_ = elements * sizeof(T);

    // allocate(1, elements) fills inputs_[0] with the stride pattern:
    //   inputs_[0][i] = i * (worldSize * 1) + (rank * 1 + 0) = i*worldSize + rank
    // Uses aligned_allocator, matching the framework convention used by
    // AllgatherBenchmark, BroadcastBenchmark, etc.
    auto inPtrs = this->allocate(1, elements);

    // Zeroed receive buffers for every rank that is not this rank.
    // The allgather will overwrite these with the sender's data.
    recvBufs_.assign(worldSize, std::vector<T>(elements, T(0)));

    // Build the flat pointer array required by PeelAllgather::run().
    bufPtrs_.resize(worldSize);
    for (int r = 0; r < worldSize; ++r) {
      bufPtrs_[r] = (r == rank)
                        ? static_cast<void*>(inPtrs[0])             // aligned send buf
                        : static_cast<void*>(recvBufs_[r].data());  // zeroed recv buf
    }

    // Initialize shared contexts and allgather object once.
    std::lock_guard<std::mutex> lock(initMutex_);
    if (sharedAllgather_) {
      return;
    }

    transport::peel::PeelDiscoveryConfig dc;
    dc.rank         = rank;
    dc.world_size   = worldSize;
    dc.redis_host   = this->options_.redisHost;
    dc.redis_port   = this->options_.redisPort;
    dc.redis_prefix = this->options_.prefix + "/peel_ag_ip";
    dc.iface_name   = this->options_.peelIface;
    dc.timeout_ms   = 300000;

    transport::peel::PeelDiscovery discovery(dc);
    GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

    sharedCtxs_.resize(worldSize);
    std::vector<transport::peel::PeelContext*> ctxPtrs(worldSize);

    for (int r = 0; r < worldSize; ++r) {
      transport::peel::PeelContextConfig cfg;
      cfg.rank          = rank;
      cfg.world_size    = worldSize;
      cfg.sender_rank   = r;
      cfg.peer_ips      = discovery.peerIps();
      cfg.mcast_group   = this->options_.peelMcastGroup;
      cfg.base_port     = static_cast<uint16_t>(this->options_.peelBasePort);
      cfg.iface_name    = this->options_.peelIface;
      cfg.ttl           = this->options_.peelTTL;
      cfg.topology_file = this->options_.peelTopologyFile;
      cfg.rto_ms        = this->options_.peelRtoMs;
      cfg.max_chunk_size = static_cast<size_t>(this->options_.peelMaxPayload);

      sharedCtxs_[r] = std::make_shared<transport::peel::PeelContext>(cfg);
      GLOO_ENFORCE(
          sharedCtxs_[r]->init(),
          "PeelContext init failed for sender_rank=", r);
      ctxPtrs[r] = sharedCtxs_[r].get();
    }

    auto mode = this->options_.peelParallel
                    ? transport::peel::PeelAllgatherMode::Parallel
                    : transport::peel::PeelAllgatherMode::Sequential;

    sharedAllgather_ = std::make_shared<transport::peel::PeelAllgather>(
        ctxPtrs, mode);
  }

  void run() override {
    sharedAllgather_->run(bufPtrs_, dataBytes_);
  }

  void verify(std::vector<std::string>& errors) override {
    const int rank = this->context_->rank;
    const int worldSize = this->context_->size;
    // inputs_ is populated by allocate(); inputs_[0].size() == elements.
    const size_t elements = this->inputs_.empty() ? 0 : this->inputs_[0].size();
    // stride matches allocate(1, elements): worldSize * numInputs = worldSize * 1
    const int stride = worldSize;

    // float16 has no operator float() or operator int() — use operator<< which
    // IS defined for all T (float16's is in types.h, float/char use stdlib).
    auto toStr = [](T v) -> std::string {
      std::ostringstream oss;
      oss << v;
      return oss.str();
    };

    for (int r = 0; r < worldSize; ++r) {
      // Use the aligned inputs_ buffer for our own rank; recvBufs_ for others.
      const T* buf = (r == rank)
                         ? this->inputs_[0].data()
                         : reinterpret_cast<const T*>(bufPtrs_[r]);
      for (size_t i = 0; i < elements; ++i) {
        // allocate() pattern for sender rank r: buf[i] = i * stride + r
        T expected = static_cast<T>(static_cast<int>(i) * stride + r);
        if (buf[i] != expected) {
          errors.push_back(
              "peel_allgather rank=" + std::to_string(rank) +
              " buf[sender=" + std::to_string(r) + "][" +
              std::to_string(i) + "]=" + toStr(buf[i]) +
              " expected=" + toStr(expected));
          break; // report first mismatch per buffer, then move on
        }
      }
    }
  }
};

template <typename T>
std::shared_ptr<transport::peel::PeelContext>
    PeelBroadcastRingBenchmark<T>::sharedCtx_;

template <typename T>
std::mutex PeelBroadcastRingBenchmark<T>::initMutex_;


template <typename T>
class PeelBroadcastStopAndWaitBenchmark : public Benchmark<T> {
  static std::shared_ptr<transport::peel::PeelContext> sharedCtx_;
  static std::mutex initMutex_;

 public:
  PeelBroadcastStopAndWaitBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options) {}

  void initialize(size_t elements) override {
    GLOO_ENFORCE(
        !this->options_.peelIface.empty(),
        "broadcast_stop_and_wait requires --peel-iface");
    GLOO_ENFORCE(
        this->options_.threads == 1,
        "broadcast_stop_and_wait does not support --threads > 1 "
        "(shared PeelContext is not safe for concurrent broadcasts)");
    GLOO_ENFORCE(
        this->options_.iterationCount > 0,
        "broadcast_stop_and_wait requires --iteration-count N "
        "(auto iteration scaling uses gloo TCP broadcast which times out "
        "with asymmetric subtrees)");

    this->allocate(this->options_.inputs, elements);

    std::lock_guard<std::mutex> lock(initMutex_);
    if (sharedCtx_) {
      return;
    }

    transport::peel::PeelDiscoveryConfig dc;
    dc.rank         = this->context_->rank;
    dc.world_size   = this->context_->size;
    dc.redis_host   = this->options_.redisHost;
    dc.redis_port   = this->options_.redisPort;
    dc.redis_prefix = this->options_.prefix + "/peel_saw_ip";
    dc.iface_name   = this->options_.peelIface;
    dc.timeout_ms   = 300000;

    transport::peel::PeelDiscovery discovery(dc);
    GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

    transport::peel::PeelContextConfig cfg;
    cfg.rank          = this->context_->rank;
    cfg.world_size    = this->context_->size;
    cfg.peer_ips      = discovery.peerIps();
    cfg.mcast_group   = this->options_.peelMcastGroup;
    cfg.base_port     = static_cast<uint16_t>(this->options_.peelBasePort);
    cfg.iface_name    = this->options_.peelIface;
    cfg.ttl           = this->options_.peelTTL;
    cfg.sender_rank   = this->options_.peelSenderRank;
    cfg.topology_file = this->options_.peelTopologyFile;
    cfg.rto_ms        = this->options_.peelRtoMs;
    cfg.max_chunk_size = static_cast<size_t>(this->options_.peelMaxPayload);

    sharedCtx_ = std::make_shared<transport::peel::PeelContext>(cfg);
    GLOO_ENFORCE(
        sharedCtx_->initStopAndWait(),
        "PeelContext stop-and-wait init failed");
  }

  void run() override {
    GLOO_ENFORCE(
        sharedCtx_->broadcastStopAndWait(
            this->options_.peelSenderRank,
            this->inputs_[0].data(),
            this->inputs_[0].size() * sizeof(T)),
        "Peel stop-and-wait broadcast failed");
  }

  void verify(std::vector<std::string>& errors) override {
    const auto stride = this->context_->size * this->inputs_.size();
    constStrideVerify(
        this->inputs_,
        this->options_.peelSenderRank,
        stride,
        this->context_->rank,
        errors);
  }

};

template <typename T>
std::shared_ptr<transport::peel::PeelContext>
    PeelBroadcastStopAndWaitBenchmark<T>::sharedCtx_;

template <typename T>
std::mutex PeelBroadcastStopAndWaitBenchmark<T>::initMutex_;

template <typename T>
std::shared_ptr<transport::peel::PeelAllgather>
    PeelAllgatherBenchmark<T>::sharedAllgather_;

template <typename T>
std::vector<std::shared_ptr<transport::peel::PeelContext>>
    PeelAllgatherBenchmark<T>::sharedCtxs_;

template <typename T>
std::mutex PeelAllgatherBenchmark<T>::initMutex_;

// =============================================================================
// PeelAllgatherRingBenchmark
//
// Ring allgather over the single-receiver Peel hop transports created by
// PeelContext::initRing().  This follows the same round-by-round dataflow as
// Gloo's AllgatherRing, but each logical edge uses a dedicated 2-rank Peel
// transport: sender i -> receiver (i + 1) % worldSize.
// =============================================================================
template <typename T>
class PeelAllgatherRingBenchmark : public Benchmark<T> {
  using Benchmark<T>::Benchmark;

  static std::shared_ptr<transport::peel::PeelContext> sharedCtx_;
  static std::mutex initMutex_;

  std::vector<std::vector<T>> recvBufs_;
  std::vector<void*> bufPtrs_;
  size_t dataBytes_ = 0;

 public:
  void initialize(size_t elements) override {
    GLOO_ENFORCE(
        !this->options_.peelIface.empty(),
        "peel_allgather_ring requires --peel-iface");
    GLOO_ENFORCE(
        this->options_.threads == 1,
        "peel_allgather_ring does not support --threads > 1 ");
    GLOO_ENFORCE(
        this->options_.iterationCount > 0,
        "peel_allgather_ring requires --iteration-count N ");

    const int rank = this->context_->rank;
    const int worldSize = this->context_->size;
    dataBytes_ = elements * sizeof(T);

    auto inPtrs = this->allocate(1, elements);

    recvBufs_.assign(worldSize, std::vector<T>(elements, T(0)));
    bufPtrs_.resize(worldSize);
    for (int r = 0; r < worldSize; ++r) {
      bufPtrs_[r] = (r == rank)
                        ? static_cast<void*>(inPtrs[0])
                        : static_cast<void*>(recvBufs_[r].data());
    }

    std::lock_guard<std::mutex> lock(initMutex_);
    if (sharedCtx_) {
      return;
    }

    transport::peel::PeelDiscoveryConfig dc;
    dc.rank         = rank;
    dc.world_size   = worldSize;
    dc.redis_host   = this->options_.redisHost;
    dc.redis_port   = this->options_.redisPort;
    dc.redis_prefix = this->options_.prefix + "/peel_ag_ring_ip";
    dc.iface_name   = this->options_.peelIface;
    dc.timeout_ms   = 300000;

    transport::peel::PeelDiscovery discovery(dc);
    GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

    transport::peel::PeelContextConfig cfg;
    cfg.rank          = rank;
    cfg.world_size    = worldSize;
    cfg.peer_ips      = discovery.peerIps();
    cfg.mcast_group   = this->options_.peelMcastGroup;
    cfg.base_port     = static_cast<uint16_t>(this->options_.peelBasePort);
    cfg.iface_name    = this->options_.peelIface;
    cfg.ttl           = this->options_.peelTTL;
    cfg.sender_rank   = 0;
    cfg.topology_file = this->options_.peelTopologyFile;
    cfg.rto_ms        = this->options_.peelRtoMs;
    cfg.max_chunk_size = static_cast<size_t>(this->options_.peelMaxPayload);

    sharedCtx_ = std::make_shared<transport::peel::PeelContext>(cfg);
    GLOO_ENFORCE(sharedCtx_->initRing(), "PeelContext ring init failed");
  }

  void run() override {
    GLOO_ENFORCE(
        sharedCtx_->allgatherRing(bufPtrs_, dataBytes_),
        "Peel ring allgather failed");
  }

  void verify(std::vector<std::string>& errors) override {
    const int rank = this->context_->rank;
    const int worldSize = this->context_->size;
    const size_t elements = this->inputs_.empty() ? 0 : this->inputs_[0].size();
    const int stride = worldSize;

    auto toStr = [](T v) -> std::string {
      std::ostringstream oss;
      oss << v;
      return oss.str();
    };

    for (int r = 0; r < worldSize; ++r) {
      const T* buf = (r == rank)
                         ? this->inputs_[0].data()
                         : reinterpret_cast<const T*>(bufPtrs_[r]);
      for (size_t i = 0; i < elements; ++i) {
        T expected = static_cast<T>(static_cast<int>(i) * stride + r);
        if (buf[i] != expected) {
          errors.push_back(
              "peel_allgather_ring rank=" + std::to_string(rank) +
              " buf[sender=" + std::to_string(r) + "][" +
              std::to_string(i) + "]=" + toStr(buf[i]) +
              " expected=" + toStr(expected));
          break;
        }
      }
    }
  }
};

template <typename T>
std::shared_ptr<transport::peel::PeelContext>
    PeelAllgatherRingBenchmark<T>::sharedCtx_;

template <typename T>
std::mutex PeelAllgatherRingBenchmark<T>::initMutex_;

template <typename T>
class PeelAllreduceRingBenchmark : public Benchmark<T> {
  static std::shared_ptr<transport::peel::PeelContext> sharedCtx_;
  static std::mutex initMutex_;

 public:
  PeelAllreduceRingBenchmark(
      std::shared_ptr<::gloo::Context>& context,
      struct options& options)
      : Benchmark<T>(context, options), barrierOpts_(context) {
    barrierOpts_.setTag(0xBADC0DE4);
  }

  void initialize(size_t elements) override {
    GLOO_ENFORCE(
        !this->options_.peelIface.empty(),
        "peel_allreduce_ring requires --peel-iface");
    GLOO_ENFORCE(
        this->options_.threads == 1,
        "peel_allreduce_ring does not support --threads > 1 "
        "(shared PeelContext is not safe for concurrent collectives)");
    GLOO_ENFORCE(
        this->options_.iterationCount > 0,
        "peel_allreduce_ring requires --iteration-count N ");

    auto ptrs = this->allocate(this->options_.inputs, elements);

    {
      std::lock_guard<std::mutex> lock(initMutex_);
      if (!sharedCtx_) {
        const int rank = this->context_->rank;
        const int worldSize = this->context_->size;

        transport::peel::PeelDiscoveryConfig dc;
        dc.rank         = rank;
        dc.world_size   = worldSize;
        dc.redis_host   = this->options_.redisHost;
        dc.redis_port   = this->options_.redisPort;
        dc.redis_prefix = this->options_.prefix + "/peel_ar_ring_ip";
        dc.iface_name   = this->options_.peelIface;
        dc.timeout_ms   = 300000;

        transport::peel::PeelDiscovery discovery(dc);
        GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

        transport::peel::PeelContextConfig cfg;
        cfg.rank          = rank;
        cfg.world_size    = worldSize;
        cfg.peer_ips      = discovery.peerIps();
        cfg.mcast_group   = this->options_.peelMcastGroup;
        cfg.base_port     = static_cast<uint16_t>(this->options_.peelBasePort);
        cfg.iface_name    = this->options_.peelIface;
        cfg.ttl           = this->options_.peelTTL;
        cfg.sender_rank   = 0;
        cfg.topology_file = this->options_.peelTopologyFile;
        cfg.rto_ms        = this->options_.peelRtoMs;
        cfg.max_chunk_size = static_cast<size_t>(this->options_.peelMaxPayload);

        sharedCtx_ = std::make_shared<transport::peel::PeelContext>(cfg);
        GLOO_ENFORCE(sharedCtx_->initRing(), "PeelContext ring init failed");
      }
    }

    algorithm_.reset(new transport::peel::PeelAllreduceRing<T>(
        this->context_->rank, sharedCtx_->ringHops(), ptrs, elements));
  }

  void run() override {
    GLOO_ENFORCE(algorithm_ != nullptr, "Peel allreduce algorithm not initialized");
    GLOO_ENFORCE(algorithm_->run(), "Peel ring allreduce failed");
    barrier(barrierOpts_);
  }

  void verify(std::vector<std::string>& errors) override {
    const auto size = this->context_->size * this->inputs_.size();
    const auto expected = (size * (size - 1)) / 2;
    const auto stride = size * size;
    constStrideVerify(
        this->inputs_, expected, stride, this->context_->rank, errors);
  }

 protected:
  std::unique_ptr<transport::peel::PeelAllreduceRing<T>> algorithm_;
  BarrierOptions barrierOpts_;
};

template <typename T>
std::shared_ptr<transport::peel::PeelContext>
    PeelAllreduceRingBenchmark<T>::sharedCtx_;

template <typename T>
std::mutex PeelAllreduceRingBenchmark<T>::initMutex_;


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
  } else if (x.benchmark == "broadcast_ring") {                                \
    fn = [&](std::shared_ptr<::gloo::Context>& context) {                      \
      return gloo::make_unique<BroadcastRingBenchmark<T>>(context, x);         \
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
  } else if (x.benchmark == "peel_broadcast") {                                \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<PeelBroadcastBenchmark<T>>(context, x);         \
    };                                                                         \
  } else if (x.benchmark == "peel_broadcast_ring") {                           \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<PeelBroadcastRingBenchmark<T>>(context, x);     \
    };                                                                         \
  } else if (                                                                 \
      x.benchmark == "broadcast_stop_and_wait" ||                             \
      x.benchmark == "peel_broadcast_stop_and_wait") {                        \
    fn = [&](std::shared_ptr<Context>& context) {                             \
      return gloo::make_unique<PeelBroadcastStopAndWaitBenchmark<T>>(         \
          context, x);                                                        \
    };                                                                        \
  } else if (x.benchmark == "peel_allgather") {                                \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<PeelAllgatherBenchmark<T>>(context, x);         \
    };                                                                         \
  } else if (x.benchmark == "peel_allgather_ring") {                           \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<PeelAllgatherRingBenchmark<T>>(context, x);     \
    };                                                                         \
  } else if (x.benchmark == "peel_allreduce_ring") {                          \
    fn = [&](std::shared_ptr<Context>& context) {                              \
      return gloo::make_unique<PeelAllreduceRingBenchmark<T>>(context, x);     \
    };                                                                         \
  }                                                                            \
  if (!fn) {                                                                   \
    GLOO_ENFORCE(false, "Invalid algorithm: ", x.benchmark);                 \
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
