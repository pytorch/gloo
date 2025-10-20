/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/alltoallv.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Test parameterization.
using Param = std::tuple<Transport, int, size_t>;

// Test fixture.
class AlltoallvTest : public BaseTest,
                      public ::testing::WithParamInterface<Param> {};

TEST_P(AlltoallvTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    /*
     * Rank0: in lengths [4, 3, 2, 1]
     *        out lengths [4, 5, 6, 7]
     * Rank1: in lengths [5, 4, 3, 2]
     *        out lengths [3, 4, 5, 6]
     * rank2: in lengths [6, 5, 4, 3]
     *        out lengths [2, 3, 4, 5]
     * rank3: in lengths [7, 6, 5, 4]
     *        out lengths [1, 2, 3, 4]
     */

    size_t inLength = context->size * (context->rank + 1) +
        context->size * (context->size - 1) / 2;
    size_t outlength = context->size * (context->size - context->rank) +
        context->size * (context->size - 1) / 2;
    std::vector<size_t> input(inLength * dataSize);
    std::vector<size_t> output(outlength * dataSize);
    std::vector<int64_t> inElementsPerRank;
    std::vector<int64_t> outElementsPerRank;

    // Fill input buffer.
    size_t offset = 0;
    for (int i = 0; i < contextSize; i++) {
      size_t length = context->size + context->rank - i;
      for (int j = 0; j < length * dataSize; j++) {
        input[offset + j] = context->rank * j + i * 127;
      }
      offset += length * dataSize;
    }

    // Set up splits.
    for (int i = 0; i < context->size; i++) {
      inElementsPerRank.push_back(
          dataSize * (context->rank + context->size - i));
      outElementsPerRank.push_back(
          dataSize * (context->size - context->rank + i));
    }

    // Alltoallv
    AlltoallvOptions opts(context);
    opts.setInput(input.data(), inElementsPerRank);
    opts.setOutput(output.data(), outElementsPerRank);

    alltoallv(opts);

    // Validate result.
    offset = 0;
    for (int i = 0; i < contextSize; i++) {
      size_t length = context->size - context->rank + i;
      for (int j = 0; j < length * dataSize; j++) {
        ASSERT_EQ(output[offset + j], i * j + context->rank * 127);
      }
      offset += length * dataSize;
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    AlltoallDefault,
    AlltoallvTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(4, 100, 1000, 10000)));
} // namespace
} // namespace test
} // namespace gloo
