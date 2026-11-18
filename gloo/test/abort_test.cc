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

class AbortBarrierTest : public BaseTest,
                         public ::testing::WithParamInterface<NewParam> {};

TEST_P(AbortBarrierTest, Default) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    BarrierOptions opts(context);

    // Run barrier to synchronize processes after starting.
    barrier(opts);

    auto timeout = std::chrono::milliseconds(context->getTimeout());
    const auto start = syncNow<std::chrono::high_resolution_clock>(context);
    // Run barrier on all ranks but 0 so it hangs
    if (context->rank != 0) {
      barrier(opts);
    }

    // Abort should unhang the barrier
    try {
      abort();
    } catch (const Exception &e) {
      EXPECT_TRUE(strstr(e.what(), "GLOO ABORTED") != NULL);
    }

    // Expect all processes to have taken less than the timeout, as abort was
    // called
    auto stop = std::chrono::high_resolution_clock::now();
    auto delta = std::chrono::duration_cast<decltype(timeout)>(stop - start);
    ASSERT_LE(delta.count(), timeout.count() / 4);
  });
}

INSTANTIATE_TEST_CASE_P(
    AbortBarrier, AbortBarrierTest,
    ::testing::Combine(::testing::ValuesIn(kTransportsForFunctionAlgorithms),
                       ::testing::Values(1, 2, 4, 7)));

} // namespace
} // namespace test
} // namespace gloo
