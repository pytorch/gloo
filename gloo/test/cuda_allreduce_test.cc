/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <memory>
#include <vector>

#include "gloo/cuda_allreduce_bcube.h"
#include "gloo/cuda_allreduce_halving_doubling.h"
#include "gloo/cuda_allreduce_halving_doubling_pipelined.h"
#include "gloo/cuda_allreduce_ring.h"
#include "gloo/cuda_allreduce_ring_chunked.h"
#include "gloo/test/cuda_base_test.h"

namespace gloo {
namespace test {
namespace {

// Check storage lifetime at transport-buffer destruction without racing I/O.
class HostStorageCheckedBuffer : public transport::Buffer {
 public:
  HostStorageCheckedBuffer(
      std::unique_ptr<transport::Buffer> buffer,
      void* storage,
      size_t* destroyed)
      : Buffer(0, storage, 0),
        buffer_(std::move(buffer)),
        destroyed_(destroyed) {}

  ~HostStorageCheckedBuffer() override {
    unsigned int flags = 0;
    EXPECT_EQ(cudaHostGetFlags(&flags, ptr_), cudaSuccess);
    ++*destroyed_;
  }

  void send(size_t offset, size_t length, size_t roffset) override {
    buffer_->send(offset, length, roffset);
  }
  void waitRecv() override {
    buffer_->waitRecv();
  }
  void waitSend() override {
    buffer_->waitSend();
  }

 private:
  std::unique_ptr<transport::Buffer> buffer_;
  size_t* destroyed_;
};

class LifetimeCheckedAllreduce : public CudaAllreduceHalvingDoubling<float> {
 public:
  using CudaAllreduceHalvingDoubling<float>::CudaAllreduceHalvingDoubling;

  size_t checkReceiveStorage(size_t* destroyed) {
    size_t count = 0;
    auto wrap = [&](std::unique_ptr<transport::Buffer>& buffer) {
      if (!buffer) {
        return;
      }
      buffer = std::make_unique<HostStorageCheckedBuffer>(
          std::move(buffer), *recvBuf_, destroyed);
      ++count;
    };
    for (auto& buffer : recvDataBufs_) {
      wrap(buffer);
    }
    wrap(smallerBlockRecvDataBuf_);
    for (auto& buffer : largerBlockRecvDataBufs_) {
      wrap(buffer);
    }
    return count;
  }
};

TEST_F(CudaBaseTest, HalvingDoublingReceiveStorageLifetime) {
  // Cover both power-of-two and unequal binary-block receive buffers.
  for (int size : {2, 3, 6}) {
    spawn(Transport::TCP, size, [&](std::shared_ptr<Context> context) {
      CudaFixture<float> fixture(context, 1, 64);
      size_t destroyed = 0;
      auto algorithm = std::make_unique<LifetimeCheckedAllreduce>(
          context, fixture.getCudaPointers(), 64, fixture.getCudaStreams());
      const auto expected = algorithm->checkReceiveStorage(&destroyed);
      EXPECT_GT(expected, 0);
      algorithm.reset();
      EXPECT_EQ(destroyed, expected);
    });
  }
}

// Function to instantiate and run algorithm.
using Func = std::unique_ptr<::gloo::Algorithm>(
    std::shared_ptr<::gloo::Context>&,
    std::vector<float*> ptrs,
    int count,
    std::vector<cudaStream_t> streams);

using Func16 = std::unique_ptr<::gloo::Algorithm>(
    std::shared_ptr<::gloo::Context>&,
    std::vector<float16*> ptrs,
    int count,
    std::vector<cudaStream_t> streams);

// Test parameterization.
using Param = std::tuple<Transport, int, int, std::function<Func>, int>;
using ParamHP = std::tuple<Transport, int, int, std::function<Func16>>;

// Test case
class CudaAllreduceTest : public CudaBaseTest,
                          public ::testing::WithParamInterface<Param> {
 public:
  void assertResult(CudaFixture<float>& fixture) {
    fixture.copyToHost();
    fixture.checkAllreduceResult();
  }
};

class CudaAllreduceTestHP : public CudaBaseTest,
                            public ::testing::WithParamInterface<ParamHP> {
 public:
  void assertResult(CudaFixture<float16>& fixture) {
    fixture.copyToHost();
    fixture.checkAllreduceResult();
  }
};

static std::function<Func> allreduceRing =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceRing<float>(context, ptrs, count, streams));
    };

static std::function<Func16> allreduceRingHP =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float16*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceRing<float16>(
              context, ptrs, count, streams));
    };

static std::function<Func> allreduceRingChunked =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceRingChunked<float>(
              context, ptrs, count, streams));
    };

static std::function<Func16> allreduceRingChunkedHP =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float16*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceRingChunked<float16>(
              context, ptrs, count, streams));
    };

static std::function<Func> allreduceHalvingDoubling =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceHalvingDoubling<float>(
              context, ptrs, count, streams));
    };

static std::function<Func> allreduceBcube =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceBcube<float>(context, ptrs, count, streams));
    };

static std::function<Func16> allreduceHalvingDoublingHP =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float16*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceHalvingDoubling<float16>(
              context, ptrs, count, streams));
    };

static std::function<Func> allreduceHalvingDoublingPipelined =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceHalvingDoublingPipelined<float>(
              context, ptrs, count, streams));
    };

static std::function<Func16> allreduceHalvingDoublingPipelinedHP =
    [](std::shared_ptr<::gloo::Context>& context,
       std::vector<float16*> ptrs,
       int count,
       std::vector<cudaStream_t> streams) {
      return std::unique_ptr<::gloo::Algorithm>(
          new ::gloo::CudaAllreduceHalvingDoublingPipelined<float16>(
              context, ptrs, count, streams));
    };

TEST_P(CudaAllreduceTest, SinglePointer) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());
  const auto fn = std::get<3>(GetParam());
  const auto base = std::get<4>(GetParam());

  spawn(
      transport,
      contextSize,
      [&](std::shared_ptr<Context> context) {
        // Run algorithm
        auto fixture = CudaFixture<float>(context, 1, dataSize);
        auto ptrs = fixture.getCudaPointers();
        auto algorithm = fn(context, ptrs, dataSize, {});
        fixture.assignValues();
        algorithm->run();

        // Verify result
        assertResult(fixture);
      },
      base);
}

TEST_P(CudaAllreduceTest, MultiPointer) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());
  const auto fn = std::get<3>(GetParam());
  const auto base = std::get<4>(GetParam());

  spawn(
      transport,
      contextSize,
      [&](std::shared_ptr<Context> context) {
        // Run algorithm
        auto fixture = CudaFixture<float>(context, cudaNumDevices(), dataSize);
        auto ptrs = fixture.getCudaPointers();
        auto algorithm = fn(context, ptrs, dataSize, {});
        fixture.assignValues();
        algorithm->run();

        // Verify result
        assertResult(fixture);
      },
      base);
}

TEST_P(CudaAllreduceTest, MultiPointerAsync) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());
  const auto fn = std::get<3>(GetParam());
  const auto base = std::get<4>(GetParam());

  spawn(
      transport,
      contextSize,
      [&](std::shared_ptr<Context> context) {
        // Run algorithm
        auto fixture = CudaFixture<float>(context, cudaNumDevices(), dataSize);
        auto ptrs = fixture.getCudaPointers();
        auto streams = fixture.getCudaStreams();
        auto algorithm = fn(context, ptrs, dataSize, streams);
        fixture.assignValuesAsync();
        algorithm->run();

        // Verify result
        fixture.synchronizeCudaStreams();
        assertResult(fixture);
      },
      base);
}

TEST_F(CudaAllreduceTest, MultipleAlgorithms) {
  const auto contextSize = 4;
  const auto dataSize = 1000;
  const auto fns = {
      allreduceRing,
      allreduceRingChunked,
      allreduceHalvingDoubling,
      allreduceHalvingDoublingPipelined,
  };

  spawn(Transport::TCP, contextSize, [&](std::shared_ptr<Context> context) {
    for (const auto& fn : fns) {
      // Run algorithm
      auto fixture = CudaFixture<float>(context, 1, dataSize);
      auto ptrs = fixture.getCudaPointers();

      auto algorithm = fn(context, ptrs, dataSize, {});
      fixture.assignValues();
      algorithm->run();

      // Verify result
      assertResult(fixture);

      auto algorithm2 = fn(context, ptrs, dataSize, {});
      fixture.assignValues();
      algorithm2->run();

      // Verify result
      assertResult(fixture);
    }
  });
}

TEST_F(CudaAllreduceTestHP, HalfPrecisionTest) {
  const auto contextSize = 4;
  const auto dataSize = 128;
  const auto fns = {
      allreduceRingHP,
      allreduceRingChunkedHP,
      allreduceHalvingDoublingHP,
      allreduceHalvingDoublingPipelinedHP,
  };

  spawn(Transport::TCP, contextSize, [&](std::shared_ptr<Context> context) {
    for (const auto& fn : fns) {
      // Run algorithm
      auto fixture = CudaFixture<float16>(context, 1, dataSize);
      auto ptrs = fixture.getCudaPointers();

      auto algorithm = fn(context, ptrs, dataSize, {});
      fixture.assignValues();
      algorithm->run();

      // Verify result
      assertResult(fixture);
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    AllreduceRing,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Range(1, 16),
        ::testing::Values(4, 100, 1000, 10000),
        ::testing::Values(allreduceRing),
        ::testing::Values(0)));

INSTANTIATE_TEST_CASE_P(
    AllreduceRingChunked,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Range(1, 16),
        ::testing::Values(4, 100, 1000, 10000),
        ::testing::Values(allreduceRingChunked),
        ::testing::Values(0)));

INSTANTIATE_TEST_CASE_P(
    AllreduceHalvingDoubling,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 16, 24, 32),
        ::testing::Values(1, 64, 1000),
        ::testing::Values(allreduceHalvingDoubling),
        ::testing::Values(0)));

INSTANTIATE_TEST_CASE_P(
    AllreduceBcubeBase2,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Values(1, 2, 4, 8, 16),
        ::testing::Values(1, 64, 1000),
        ::testing::Values(allreduceBcube),
        ::testing::Values(2)));

INSTANTIATE_TEST_CASE_P(
    AllreduceBcubeBase3,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Values(1, 3, 9, 27),
        ::testing::Values(1, 64, 1000),
        ::testing::Values(allreduceBcube),
        ::testing::Values(3)));

INSTANTIATE_TEST_CASE_P(
    AllreduceBcubeBase4,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Values(1, 4, 16),
        ::testing::Values(1, 64, 1000),
        ::testing::Values(allreduceBcube),
        ::testing::Values(4)));

INSTANTIATE_TEST_CASE_P(
    AllreduceHalvingDoublingPipelined,
    CudaAllreduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 16, 24, 32),
        ::testing::Values(1, 64, 1000),
        ::testing::Values(allreduceHalvingDoublingPipelined),
        ::testing::Values(0)));

} // namespace
} // namespace test
} // namespace gloo
