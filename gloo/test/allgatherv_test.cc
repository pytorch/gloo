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

#include "gloo/allgatherv.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

using Param = std::tuple<Transport, int, int, bool>;

class AllgathervTest : public BaseTest,
                       public ::testing::WithParamInterface<Param> {};

TEST_P(AllgathervTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());
  const auto passBuffers = std::get<3>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    // This test uses the same output size for every iteration,
    // but assigns different counts to different ranks.
    std::vector<uint64_t> output(
        dataSize * (context->size * (context->size - 1)) / 2);
    std::vector<uint64_t> input(dataSize * context->size);
    std::vector<size_t> counts(context->size);

    // Use each rank as a base once.
    for (auto i = 0; i < context->size; i++) {
      // Initialize counts per rank.
      for (auto j = 0; j < context->size; j++) {
        counts[(i + j) % context->size] = j * dataSize;
      }

      // Count for this process's rank.
      const auto count =
          ((context->size - i + context->rank) % context->size) * dataSize;

      // Run with raw pointers and sizes in options.
      AllgathervOptions opts(context);
      std::fill(input.begin(), input.end(), context->rank);
      if (passBuffers) {
        opts.setInput<uint64_t>(context->createUnboundBuffer(
            input.data(), count * sizeof(uint64_t)));
      } else {
        opts.setInput(input.data(), count);
      }
      std::fill(output.begin(), output.end(), UINT64_MAX);
      if (passBuffers) {
        opts.setOutput<uint64_t>(
            context->createUnboundBuffer(
                output.data(), output.size() * sizeof(uint64_t)),
            counts);
      } else {
        opts.setOutput(output.data(), counts);
      }
      allgatherv(opts);

      // Verify output.
      size_t offset = 0;
      for (auto j = 0; j < context->size; j++) {
        for (auto k = 0; k < counts[j]; k++) {
          ASSERT_EQ(j, output[offset + k])
              << "Mismatch at offset=" << offset << ", k=" << k;
        }
        offset += counts[j];
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    AllgathervDefault,
    AllgathervTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(0, 1, 10, 100, 1000),
        ::testing::Values(false, true)));

TEST_F(AllgathervTest, TestTimeout) {
  spawn(Transport::TCP, 2, [&](std::shared_ptr<Context> context) {
    Fixture<uint64_t> output(context, 1, context->size);
    std::vector<size_t> counts({1, 1});
    AllgathervOptions opts(context);
    opts.setOutput(output.getPointer(), counts);

    // Run one operation first so we're measuring the operation timeout not
    // connection timeout.
    allgatherv(opts);

    opts.setTimeout(std::chrono::milliseconds(10));
    if (context->rank == 0) {
      try {
        allgatherv(opts);
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
