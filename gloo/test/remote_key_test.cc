/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <array>
#include <functional>
#include <vector>

#include "gloo/allgather.h"
#include "gloo/common/common.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {
namespace {

// Test parameterization.
using Param = std::tuple<Transport, int, int>;

std::vector<std::unique_ptr<gloo::transport::RemoteKey>> exchangeKeys(
    std::shared_ptr<Context>& context,
    const std::unique_ptr<gloo::transport::RemoteKey>& key) {
  auto selfRemoteKey = key->serialize();
  GLOO_ENFORCE_GT(selfRemoteKey.size(), 0);

  std::array<char, 1024> keyBuf;
  for (int i = 0; i < selfRemoteKey.size(); ++i) {
    keyBuf[i] = selfRemoteKey[i];
  }
  keyBuf[selfRemoteKey.size()] = '\0';

  std::unique_ptr<char[]> outPtr{
      gloo::make_unique<char[]>(keyBuf.size() * context->size)};

  AllgatherOptions opts(context);
  opts.setInput(&keyBuf[0], keyBuf.size());
  opts.setOutput(outPtr.get(), keyBuf.size() * context->size);
  allgather(opts);

  std::vector<std::unique_ptr<gloo::transport::RemoteKey>> remoteKeys;

  for (int i = 0; i < context->size; i++) {
    std::string remoteKeyStr{&outPtr[i * keyBuf.size()]};
    GLOO_ENFORCE_GT(remoteKeyStr.size(), 0);
    auto remoteKey = context->deserializeRemoteKey(remoteKeyStr);
    GLOO_ENFORCE(remoteKey.get() != nullptr);

    remoteKeys.push_back(std::move(remoteKey));
  }

  return remoteKeys;
}

// Test fixture.
class RemoteKeyTest : public BaseTest,
                      public ::testing::WithParamInterface<Param> {};

TEST_P(RemoteKeyTest, Get) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  const auto dataSize = std::get<2>(GetParam());

  Barrier barrier(contextSize);

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    int rank = context->rank;
    std::unique_ptr<char[]> sharedPtr{gloo::make_unique<char[]>(dataSize)};
    for (int i = 0; i < dataSize; ++i) {
      sharedPtr[i] = rank;
    }
    auto sharedBuf = context->createUnboundBuffer(sharedPtr.get(), dataSize);

    std::unique_ptr<char[]> localPtr{gloo::make_unique<char[]>(dataSize)};
    auto localBuf = context->createUnboundBuffer(localPtr.get(), dataSize);

    auto remoteKeys = exchangeKeys(context, sharedBuf->getRemoteKey());

    for (int i = 0; i < contextSize; ++i) {
      if (i == rank) {
        continue;
      }

      localBuf->get(*remoteKeys.at(i), context->nextSlot(), 0, 0, dataSize);
      localBuf->waitSend();

      for (int j = 0; j < dataSize; ++j) {
        ASSERT_EQ(localPtr[j], i);
      }
    }

    barrier.wait();

    // Do bound checking
    auto testRank = (rank + 1) % contextSize;
    auto& testKey = remoteKeys.at(testRank);
    EXPECT_THROW(
        localBuf->get(*testKey, context->nextSlot(), 1000000000, 0, 1),
        gloo::EnforceNotMet);
    EXPECT_THROW(
        localBuf->get(*testKey, context->nextSlot(), 0, 1000000000, 1),
        gloo::EnforceNotMet);
    EXPECT_THROW(
        localBuf->get(*testKey, context->nextSlot(), 0, 0, 1000000000),
        gloo::EnforceNotMet);
  });
}

TEST_P(RemoteKeyTest, Put) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  Barrier barrier(contextSize);

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    int rank = context->rank;
    std::unique_ptr<char[]> exportPtr{gloo::make_unique<char[]>(contextSize)};
    auto exportBuf = context->createUnboundBuffer(exportPtr.get(), contextSize);

    std::unique_ptr<char[]> localPtr{gloo::make_unique<char[]>(contextSize)};
    for (int i = 0; i < contextSize; ++i) {
      localPtr[i] = rank;
    }
    auto localBuf = context->createUnboundBuffer(localPtr.get(), contextSize);

    auto remoteKeys = exchangeKeys(context, exportBuf->getRemoteKey());

    for (int i = 0; i < contextSize; ++i) {
      if (i == rank) {
        continue;
      }

      localBuf->put(*remoteKeys.at(i), context->nextSlot(), rank, rank, 1);
      localBuf->waitSend();
    }

    barrier.wait();

    for (int j = 0; j < contextSize; ++j) {
      if (j == rank) {
        continue;
      }
      ASSERT_EQ(exportPtr[j], j);
    }

    // Do bound checking
    auto testRank = (rank + 1) % contextSize;
    auto& testKey = remoteKeys.at(testRank);
    EXPECT_THROW(
        localBuf->put(*testKey, context->nextSlot(), 1000000000, 0, 1),
        gloo::EnforceNotMet);
    EXPECT_THROW(
        localBuf->put(*testKey, context->nextSlot(), 0, 1000000000, 1),
        gloo::EnforceNotMet);
    EXPECT_THROW(
        localBuf->put(*testKey, context->nextSlot(), 0, 0, 1000000000),
        gloo::EnforceNotMet);
  });
}

INSTANTIATE_TEST_CASE_P(
    RemoteKeyTestBasics,
    RemoteKeyTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForRDMA),
        ::testing::Values(2, 4),
        ::testing::Values(0, 1024, 1000000)));

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RemoteKeyTest);

} // namespace
} // namespace test
} // namespace gloo
