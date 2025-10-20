/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/gather.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Test parameterization.
using Param = std::tuple<Transport, int, size_t>;

// Test fixture.
class GatherTest : public BaseTest,
                   public ::testing::WithParamInterface<Param> {};

TEST_P(GatherTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    auto input = Fixture<uint64_t>(context, 1, dataSize);
    auto output = Fixture<uint64_t>(context, 1, contextSize * dataSize);

    // Initialize fixture with globally unique values
    input.assignValues();

    GatherOptions opts(context);
    opts.setInput(input.getPointer(), dataSize);

    // Take turns being root
    for (auto i = 0; i < context->size; i++) {
      // Set output pointer only when root
      if (i == context->rank) {
        opts.setOutput(output.getPointer(), dataSize * contextSize);
      }

      opts.setRoot(i);
      gather(opts);

      // Validate result if root
      if (i == context->rank) {
        const auto ptr = output.getPointer();
        const auto stride = context->size;
        for (auto j = 0; j < context->size; j++) {
          for (auto k = 0; k < dataSize; k++) {
            ASSERT_EQ(j + k * stride, ptr[k + j * dataSize])
                << "Mismatch at index " << (k + j * dataSize);
          }
        }
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    GatherDefault,
    GatherTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(4, 100, 1000, 10000)));

TEST_F(GatherTest, TestTimeout) {
  spawn(Transport::TCP, 2, [&](std::shared_ptr<Context> context) {
    Fixture<uint64_t> input(context, 1, 1);
    Fixture<uint64_t> output(context, 1, context->size);
    GatherOptions opts(context);
    opts.setInput(input.getPointer(), 1);
    opts.setOutput(output.getPointer(), context->size);
    opts.setRoot(0);

    // Run one operation first so we're measuring the operation timeout not
    // connection timeout.
    gather(opts);

    opts.setTimeout(std::chrono::milliseconds(10));
    if (context->rank == 0) {
      try {
        gather(opts);
        FAIL() << "Expected exception to be thrown";
      } catch (::gloo::IoException& e) {
        ASSERT_NE(std::string(e.what()).find("Timed out"), std::string::npos);
      }
    }
  });
}

} // namespace
} // namespace test
} // namespace gloo
