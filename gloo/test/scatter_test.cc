/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/math.h"
#include "gloo/scatter.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Test parameterization.
using Param = std::tuple<Transport, int, size_t>;

// Test fixture.
class ScatterTest : public BaseTest,
                    public ::testing::WithParamInterface<Param> {};

TEST_P(ScatterTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    auto input = Fixture<uint64_t>(context, contextSize, dataSize);
    auto output = Fixture<uint64_t>(context, 1, dataSize);

    ScatterOptions opts(context);

    // Multiple inputs (one per rank)
    opts.setInputs(input.getPointers(), dataSize);

    // Single output
    opts.setOutput(output.getPointer(), dataSize);

    // Take turns being root
    for (size_t root = 0; root < context->size; root++) {
      input.assignValues();
      output.clear();
      opts.setRoot(root);
      scatter(opts);

      // Validate result on all processes
      const auto ptr = output.getPointer();
      const auto base = (root * context->size) + context->rank;
      const auto stride = context->size * context->size;
      for (auto j = 0; j < dataSize; j++) {
        ASSERT_EQ(j * stride + base, ptr[j]) << "Mismatch at index " << j;
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    ScatterDefault,
    ScatterTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(1, 10, 100)));

TEST_F(ScatterTest, TestTimeout) {
  spawn(Transport::TCP, 2, [&](std::shared_ptr<Context> context) {
    Fixture<uint64_t> input(context, context->size, 1);
    Fixture<uint64_t> output(context, 1, 1);
    ScatterOptions opts(context);
    opts.setInputs(input.getPointers(), 1);
    opts.setOutput(output.getPointer(), 1);
    opts.setRoot(0);

    // Run one operation first so we're measuring the operation timeout not
    // connection timeout.
    scatter(opts);

    opts.setTimeout(std::chrono::milliseconds(10));
    if (context->rank == 0) {
      try {
        scatter(opts);
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
