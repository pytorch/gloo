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

#include "gloo/test/multiproc_test.h"

namespace gloo {
namespace test {
namespace {

const std::vector<Transport> kTransportsForMultiProcTest{
#if GLOO_HAVE_TRANSPORT_TCP
    Transport::TCP,
#endif
#if GLOO_HAVE_TRANSPORT_TCP_TLS
    Transport::TCP_TLS,
#endif
};

enum IoMode { Async, Blocking, Polling };

// Test parameterization.
using Param = std::tuple<int, int, int, IoMode, Transport>;

// Test fixture.
class TransportMultiProcTest : public MultiProcTest,
                               public ::testing::WithParamInterface<Param> {};

static void setMode(std::unique_ptr<transport::Pair>& pair, IoMode mode) {
  switch (mode) {
    case IoMode::Async:
      // Async is default mode
      break;
    case IoMode::Blocking:
      pair->setSync(true, false);
      break;
    case IoMode::Polling:
      pair->setSync(true, true);
      break;
    default:
      FAIL();
  }
}

TEST_P(TransportMultiProcTest, IoErrors) {
  const auto processCount = std::get<0>(GetParam());
  const auto elementCount = std::get<1>(GetParam());
  const auto sleepMs = std::get<2>(GetParam());
  const auto mode = std::get<3>(GetParam());
  const auto transport = std::get<4>(GetParam());

  spawnAsync(transport, processCount, [&](std::shared_ptr<Context> context) {
    std::vector<float> data;
    data.resize(elementCount);
    std::unique_ptr<transport::Buffer> sendBuffer;
    std::unique_ptr<transport::Buffer> recvBuffer;

    const auto& leftRank = (processCount + context->rank - 1) % processCount;
    auto& left = context->getPair(leftRank);
    setMode(left, mode);
    recvBuffer =
        left->createRecvBuffer(0, data.data(), data.size() * sizeof(float));

    const auto& rightRank = (context->rank + 1) % processCount;
    auto& right = context->getPair(rightRank);
    setMode(right, mode);
    sendBuffer =
        right->createSendBuffer(0, data.data(), data.size() * sizeof(float));

    while (true) {
      // Send value to the remote buffer
      sendBuffer->send(0, sizeof(float));
      sendBuffer->waitSend();

      // Wait for receive
      recvBuffer->waitRecv();
    }
  });
  if (sleepMs > 0) {
    // The test is specifically delaying before killing dependent processes.
    // The absolute time does not need to be deterministic.
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  }

  // Kill one of the processes and wait for all to exit.
  // Expect this to take less time than the default timeout.
  const auto start = std::chrono::high_resolution_clock::now();
  signalProcess(0, SIGKILL);
  wait();
  const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::high_resolution_clock::now() - start);
  ASSERT_LT(delta.count(), kMultiProcTimeout.count() * 2);

  for (auto i = 0; i < processCount; i++) {
    if (i != 0) {
      const auto result = getResult(i);
      ASSERT_TRUE(WIFEXITED(result)) << result;
      ASSERT_EQ(kExitWithIoException, WEXITSTATUS(result));
    }
  }
}

TEST_P(TransportMultiProcTest, IoTimeouts) {
  const auto processCount = std::get<0>(GetParam());
  const auto elementCount = std::get<1>(GetParam());
  const auto sleepMs = std::get<2>(GetParam());
  const auto transport = std::get<4>(GetParam());

  spawnAsync(transport, processCount, [&](std::shared_ptr<Context> context) {
    std::vector<float> data;
    data.resize(elementCount);
    std::unique_ptr<transport::Buffer> sendBuffer;
    std::unique_ptr<transport::Buffer> recvBuffer;

    const auto& leftRank = (processCount + context->rank - 1) % processCount;
    auto& left = context->getPair(leftRank);
    recvBuffer =
        left->createRecvBuffer(0, data.data(), data.size() * sizeof(float));

    const auto& rightRank = (context->rank + 1) % processCount;
    auto& right = context->getPair(rightRank);
    sendBuffer =
        right->createSendBuffer(0, data.data(), data.size() * sizeof(float));

    while (true) {
      // Send value to the remote buffer
      sendBuffer->send(0, sizeof(float));
      sendBuffer->waitSend();

      // Wait for receive
      recvBuffer->waitRecv();
    }
  });
  if (sleepMs > 0) {
    // The test is specifically delaying before killing dependent processes.
    // The absolute time does not need to be deterministic.
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  }

  // Stop one process and wait for the others to exit
  signalProcess(0, SIGSTOP);
  for (auto i = 0; i < processCount; i++) {
    if (i != 0) {
      waitProcess(i);
      const auto result = getResult(i);
      ASSERT_TRUE(WIFEXITED(result)) << result;
      ASSERT_EQ(kExitWithIoException, WEXITSTATUS(result));
    }
  }

  // Kill the stopped process
  signalProcess(0, SIGKILL);
  waitProcess(0);
}

TEST_P(TransportMultiProcTest, UnboundIoErrors) {
  const auto processCount = std::get<0>(GetParam());
  const auto sleepMs = std::get<2>(GetParam());
  const auto transport = std::get<4>(GetParam());

  spawnAsync(transport, processCount, [&](std::shared_ptr<Context> context) {
    int sendScratch = 0;
    int recvScratch = 0;
    auto sendBuf =
        context->createUnboundBuffer(&sendScratch, sizeof(sendScratch));
    auto recvBuf =
        context->createUnboundBuffer(&recvScratch, sizeof(recvScratch));
    const auto leftRank = (context->size + context->rank - 1) % context->size;
    const auto rightRank = (context->rank + 1) % context->size;
    while (true) {
      sendBuf->send(leftRank, 0);
      recvBuf->recv(rightRank, 0);
      sendBuf->waitSend();
      recvBuf->waitRecv();
    }
  });

  if (sleepMs > 0) {
    // The test is specifically delaying before killing dependent processes.
    // The absolute time does not need to be deterministic.
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  }

  // Kill one of the processes and wait for all to exit.
  // Expect this to take less time than the default timeout.
  const auto start = std::chrono::high_resolution_clock::now();
  signalProcess(0, SIGKILL);
  wait();
  const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::high_resolution_clock::now() - start);
  ASSERT_LT(delta.count(), kMultiProcTimeout.count() * 2);

  for (auto i = 0; i < processCount; i++) {
    if (i != 0) {
      const auto result = getResult(i);
      ASSERT_TRUE(WIFEXITED(result)) << result;
      ASSERT_EQ(kExitWithIoException, WEXITSTATUS(result));
    }
  }
}

TEST_P(TransportMultiProcTest, UnboundIoTimeout) {
  const auto processCount = std::get<0>(GetParam());
  const auto sleepMs = std::get<2>(GetParam());
  const auto transport = std::get<4>(GetParam());

  spawnAsync(transport, processCount, [&](std::shared_ptr<Context> context) {
    int sendScratch = 0;
    int recvScratch = 0;
    auto sendBuf =
        context->createUnboundBuffer(&sendScratch, sizeof(sendScratch));
    auto recvBuf =
        context->createUnboundBuffer(&recvScratch, sizeof(recvScratch));
    const auto leftRank = (context->size + context->rank - 1) % context->size;
    const auto rightRank = (context->rank + 1) % context->size;
    while (true) {
      sendBuf->send(leftRank, 0);
      recvBuf->recv(rightRank, 0);
      sendBuf->waitSend();
      recvBuf->waitRecv();
    }
  });

  if (sleepMs > 0) {
    // The test is specifically delaying before killing dependent processes.
    // The absolute time does not need to be deterministic.
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  }

  // Stop one process and wait for the others to exit.
  const auto start = std::chrono::high_resolution_clock::now();
  signalProcess(0, SIGSTOP);
  for (auto i = 0; i < processCount; i++) {
    if (i != 0) {
      waitProcess(i);
      const auto result = getResult(i);
      ASSERT_TRUE(WIFEXITED(result)) << result;
      ASSERT_EQ(kExitWithIoException, WEXITSTATUS(result));
    }
  }

  // Expect this to take more time than the default timeout.
  const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::high_resolution_clock::now() - start);
  ASSERT_GE(delta.count(), kMultiProcTimeout.count() / 2);

  // Kill the stopped process
  signalProcess(0, SIGKILL);
  waitProcess(0);
}

TEST_P(TransportMultiProcTest, UnboundIoTimeoutOverride) {
  const auto processCount = std::get<0>(GetParam());
  const auto sleepMs = std::get<2>(GetParam());
  const auto transport = std::get<4>(GetParam());

  // Use lower timeout than default and pass directly to waitSend/waitRecv.
  const auto timeout = kMultiProcTimeout;

  spawnAsync(transport, processCount, [&](std::shared_ptr<Context> context) {
    int sendScratch = 0;
    int recvScratch = 0;
    auto sendBuf =
        context->createUnboundBuffer(&sendScratch, sizeof(sendScratch));
    auto recvBuf =
        context->createUnboundBuffer(&recvScratch, sizeof(recvScratch));
    const auto leftRank = (context->size + context->rank - 1) % context->size;
    const auto rightRank = (context->rank + 1) % context->size;
    while (true) {
      sendBuf->send(leftRank, 0);
      recvBuf->recv(rightRank, 0);
      sendBuf->waitSend(timeout);
      recvBuf->waitRecv(timeout);
    }
  });

  if (sleepMs > 0) {
    // The test is specifically delaying before killing dependent processes.
    // The absolute time does not need to be deterministic.
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  }

  // Stop one process and wait for the others to exit.
  const auto start = std::chrono::high_resolution_clock::now();
  signalProcess(0, SIGSTOP);
  for (auto i = 0; i < processCount; i++) {
    if (i != 0) {
      waitProcess(i);
      const auto result = getResult(i);
      ASSERT_TRUE(WIFEXITED(result)) << result;
      ASSERT_EQ(kExitWithIoException, WEXITSTATUS(result));
    }
  }

  // Expect this to take more time than the used timeout.
  const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::high_resolution_clock::now() - start);
  ASSERT_GE(delta.count(), timeout.count() / 2);

  // Kill the stopped process
  signalProcess(0, SIGKILL);
  waitProcess(0);
}

TEST_P(TransportMultiProcTest, UnboundNoErrors) {
  const auto processCount = std::get<0>(GetParam());
  const auto transport = std::get<4>(GetParam());

  spawnAsync(transport, processCount, [&](std::shared_ptr<Context> context) {
    int sendScratch = 0;
    int recvScratch = 0;
    auto sendBuf =
        context->createUnboundBuffer(&sendScratch, sizeof(sendScratch));
    auto recvBuf =
        context->createUnboundBuffer(&recvScratch, sizeof(recvScratch));
    const auto leftRank = (context->size + context->rank - 1) % context->size;
    const auto rightRank = (context->rank + 1) % context->size;
    for (auto i = 0; i < 10; i++) {
      sendBuf->send(leftRank, 0);
      recvBuf->recv(rightRank, 0);
      sendBuf->waitSend();
      recvBuf->waitRecv();
    }

    // Number of references to the underlying device:
    // 1) Local variable
    // 2) Member variable in top level context
    // 3) Member variable in transport context
    auto device = context->getDevice();
    ASSERT_EQ(3, device.use_count());

    // Destroy the top level context before everything else.
    //
    // The unbound buffers still have a reference to the underlying
    // transport context and can continue to send/recv.
    //
    // This triggers a path where the transport context holds the
    // only reference to the device and destruction order between
    // pairs and the device is important.
    //
    ASSERT_EQ(1, context.use_count());
    context.reset();
  });

  // Wait for all processes to terminate with success.
  for (auto i = 0; i < processCount; i++) {
    waitProcess(i);
    const auto result = getResult(i);
    ASSERT_TRUE(WIFEXITED(result)) << result;
    ASSERT_EQ(0, WEXITSTATUS(result));
  }
}

std::vector<int> genMemorySizes() {
  std::vector<int> v;
  v.push_back(sizeof(float));
  v.push_back(1000);
  return v;
}

INSTANTIATE_TEST_CASE_P(
    Transport,
    TransportMultiProcTest,
    ::testing::Combine(
        ::testing::Values(2, 3, 4),
        ::testing::ValuesIn(genMemorySizes()),
        ::testing::Values(0, 5, 50),
        ::testing::Values(IoMode::Async, IoMode::Blocking, IoMode::Polling),
        ::testing::ValuesIn(kTransportsForMultiProcTest)));

} // namespace
} // namespace test
} // namespace gloo
