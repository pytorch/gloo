/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <thread>
#include <vector>

#include "gloo/gatherv.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

using Param = std::tuple<Transport, int, int, bool>;

class GathervTest : public BaseTest,
                    public ::testing::WithParamInterface<Param> {};

TEST_P(GathervTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());
  const auto passBuffers = std::get<3>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    // This test uses the same output size for every iteration,
    // but assigns different number of elements to different ranks.
    // Number of elements = dataSize * (1 + 2 + ... + context->size).
    std::vector<uint64_t> output(
        dataSize * (context->size * (context->size - 1)) / 2);
    std::vector<uint64_t> input(dataSize * context->size);
    std::vector<size_t> elements(context->size);

    // Take turns being root.
    for (auto i = 0; i < context->size; i++) {
      // Initialize elements per rank.
      for (auto j = 0; j < context->size; j++) {
        elements[(i + j) % context->size] = j * dataSize;
      }

      // Count number of elements for this process's rank.
      const auto sendElements =
          ((context->size - i + context->rank) % context->size) * dataSize;

      // Set root
      GathervOptions opts(context);
      opts.setRoot(i);

      // Set input buffer
      std::fill(input.begin(), input.end(), context->rank);
      if (passBuffers) {
        opts.setInput<uint64_t>(context->createUnboundBuffer(
            input.data(), sendElements * sizeof(uint64_t)));
      } else {
        opts.setInput<uint64_t>(input.data(), sendElements);
      }
      // Set output buffer
      std::fill(output.begin(), output.end(), UINT64_MAX);
      if (passBuffers) {
        opts.setOutput<uint64_t>(
            context->createUnboundBuffer(
                output.data(), output.size() * sizeof(uint64_t)),
            elements);
      } else {
        opts.setOutput<uint64_t>(output.data(), elements);
      }

      gatherv(opts);

      // Validate result if root.
      if (context->rank == i) {
        size_t offset = 0;
        for (auto j = 0; j < context->size; j++) {
          for (auto k = 0; k < elements[j]; k++) {
            ASSERT_EQ(j, output[offset + k])
                << "Mismatch at offset=" << offset << ", k=" << k;
          }
          offset += elements[j];
        }
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    GathervDefault,
    GathervTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(1, 10, 100, 1000),
        ::testing::Values(false, true)));

TEST_F(GathervTest, TestTimeout) {
  spawn(Transport::TCP, 2, [&](std::shared_ptr<Context> context) {
    Fixture<uint64_t> input(context, 1, 1);
    Fixture<uint64_t> output(context, 1, context->size);
    std::vector<size_t> counts({1, 1});
    GathervOptions opts(context);
    opts.setRoot(0);
    opts.setInput(input.getPointer(), 1);
    opts.setOutput(output.getPointer(), counts);

    // Run one operation first so we're measuring the operation timeout not
    // connection timeout.
    gatherv(opts);

    opts.setTimeout(std::chrono::milliseconds(10));
    if (context->rank == 0) {
      try {
        gatherv(opts);
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
