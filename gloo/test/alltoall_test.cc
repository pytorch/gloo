/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/alltoall.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Test parameterization.
using Param = std::tuple<Transport, int, size_t>;

// Test fixture.
class AlltoallTest : public BaseTest,
                     public ::testing::WithParamInterface<Param> {};

TEST_P(AlltoallTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    std::vector<uint64_t> input(contextSize * dataSize);
    std::vector<uint64_t> output(contextSize * dataSize);

    for (int i = 0; i < contextSize; i++) {
      for (int j = 0; j < dataSize; j++) {
        input[i * dataSize + j] = context->rank * j + i * 127;
      }
    }

    AlltoallOptions opts(context);
    opts.setInput(input.data(), contextSize * dataSize);
    opts.setOutput(output.data(), contextSize * dataSize);

    alltoall(opts);

    // Validate result.
    for (int i = 0; i < contextSize; i++) {
      for (int j = 0; j < dataSize; j++) {
        ASSERT_EQ(output[i * dataSize + j], i * j + context->rank * 127);
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    AlltoallDefault,
    AlltoallTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(4, 100, 1000, 10000)));
} // namespace
} // namespace test
} // namespace gloo
