/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/math.h"
#include "gloo/reduce.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Test parameterization.
using Param = std::tuple<Transport, int, size_t, bool>;

// Test fixture.
class ReduceTest : public BaseTest,
                   public ::testing::WithParamInterface<Param> {};

TEST_P(ReduceTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());
  const auto inPlace = std::get<3>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    auto input = Fixture<uint64_t>(context, 1, dataSize);
    auto output = Fixture<uint64_t>(context, 1, dataSize);

    ReduceOptions opts(context);

    if (inPlace) {
      opts.setOutput(output.getPointer(), dataSize);
    } else {
      opts.setInput(input.getPointer(), dataSize);
      opts.setOutput(output.getPointer(), dataSize);
    }

    opts.setReduceFunction([](void* a, const void* b, const void* c, size_t n) {
      auto ua = static_cast<uint64_t*>(a);
      const auto ub = static_cast<const uint64_t*>(b);
      const auto uc = static_cast<const uint64_t*>(c);
      for (size_t i = 0; i < n; i++) {
        ua[i] = ub[i] + uc[i];
      }
    });

    // A small maximum segment size triggers code paths where we'll
    // have a number of segments larger than the lower bound of
    // twice the context size.
    opts.setMaxSegmentSize(128);

    // Take turns being root
    for (size_t root = 0; root < context->size; root++) {
      if (inPlace) {
        output.assignValues();
      } else {
        input.assignValues();
        output.clear();
      }
      opts.setRoot(root);
      reduce(opts);

      // Validate result if this process was root
      if (context->rank == root) {
        const auto base = (contextSize * (contextSize - 1)) / 2;
        const auto ptr = output.getPointer();
        const auto stride = context->size;
        for (auto j = 0; j < dataSize; j++) {
          ASSERT_EQ(j * stride * stride + base, ptr[j])
              << "Mismatch at index " << j;
        }
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    ReduceDefault,
    ReduceTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7),
        ::testing::Values(0, 1, 10, 100, 1000, 10000),
        ::testing::Values(true, false)));

template <typename T>
ReduceOptions::Func getFunction() {
  void (*func)(void*, const void*, const void*, size_t) = &::gloo::sum<T>;
  return ReduceOptions::Func(func);
}

TEST_F(ReduceTest, TestTimeout) {
  spawn(Transport::TCP, 2, [&](std::shared_ptr<Context> context) {
    Fixture<uint64_t> outputs(context, 1, 1);
    ReduceOptions opts(context);
    opts.setOutput(outputs.getPointer(), 1);
    opts.setRoot(0);
    opts.setReduceFunction(getFunction<uint64_t>());

    // Run one operation first so we're measuring the operation timeout not
    // connection timeout.
    reduce(opts);

    opts.setTimeout(std::chrono::milliseconds(10));
    if (context->rank == 0) {
      try {
        reduce(opts);
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
