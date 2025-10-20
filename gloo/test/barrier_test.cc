/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <thread>
#include <vector>

#include "gloo/barrier_all_to_all.h"
#include "gloo/barrier_all_to_one.h"
#include "gloo/broadcast.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Function to instantiate and run algorithm.
using Func = void(std::shared_ptr<::gloo::Context>);

// Test parameterization.
using Param = std::tuple<Transport, int, std::function<Func>>;

// Test fixture.
class BarrierTest : public BaseTest,
                    public ::testing::WithParamInterface<Param> {};

TEST_P(BarrierTest, SinglePointer) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto fn = std::get<2>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    fn(context);
  });
}

static std::function<Func> barrierAllToAll =
    [](std::shared_ptr<::gloo::Context> context) {
      ::gloo::BarrierAllToAll algorithm(context);
      algorithm.run();
    };

INSTANTIATE_TEST_CASE_P(
    BarrierAllToAll,
    BarrierTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Range(2, 16),
        ::testing::Values(barrierAllToAll)));

static std::function<Func> barrierAllToOne =
    [](std::shared_ptr<::gloo::Context> context) {
      ::gloo::BarrierAllToOne algorithm(context);
      algorithm.run();
    };

INSTANTIATE_TEST_CASE_P(
    BarrierAllToOne,
    BarrierTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForClassAlgorithms),
        ::testing::Range(2, 16),
        ::testing::Values(barrierAllToOne)));

// Synchronized version of std::chrono::clock::now().
// All processes participating in the specified context will
// see the same value.
template <typename clock>
std::chrono::time_point<clock> syncNow(std::shared_ptr<Context> context) {
  const typename clock::time_point now = clock::now();
  typename clock::duration::rep count = now.time_since_epoch().count();
  BroadcastOptions opts(context);
  opts.setRoot(0);
  opts.setOutput(&count, 1);
  broadcast(opts);
  return typename clock::time_point(typename clock::duration(count));
}

using NewParam = std::tuple<Transport, int>;

class BarrierNewTest : public BaseTest,
                       public ::testing::WithParamInterface<NewParam> {};

TEST_P(BarrierNewTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    BarrierOptions opts(context);

    // Run barrier to synchronize processes after starting.
    barrier(opts);

    // Take turns in sleeping for a bit and checking that all processes
    // saw that artificial delay through the barrier.
    auto singleProcessDelay = std::chrono::milliseconds(10);
    for (size_t i = 0; i < context->size; i++) {
      const auto start = syncNow<std::chrono::high_resolution_clock>(context);
      if (i == context->rank) {
        /* sleep override */
        std::this_thread::sleep_for(singleProcessDelay);
      }

      barrier(opts);

      // Expect all processes to have taken at least as long as the sleep
      auto stop = std::chrono::high_resolution_clock::now();
      auto delta = std::chrono::duration_cast<decltype(singleProcessDelay)>(
          stop - start);
      ASSERT_GE(delta.count(), singleProcessDelay.count());
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    BarrierNewDefault,
    BarrierNewTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(1, 2, 4, 7)));

TEST_F(BarrierNewTest, TestTimeout) {
  spawn(Transport::TCP, 2, [&](std::shared_ptr<Context> context) {
    BarrierOptions opts(context);

    // Run barrier first so we're measuring the barrier timeout not connection
    // timeout.
    barrier(opts);

    opts.setTimeout(std::chrono::milliseconds(10));
    if (context->rank == 0) {
      try {
        barrier(opts);
        FAIL() << "Expected exception to be thrown";
      } catch (::gloo::IoException& e) {
        std::cerr << e.what() << std::endl;
        ASSERT_NE(std::string(e.what()).find("Timed out"), std::string::npos);
      }
    }
  });
}

} // namespace
} // namespace test
} // namespace gloo
