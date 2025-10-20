/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/test/base_test.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace gloo {
namespace test {
namespace {

// Test parameterization (transport, context size, buffer size).
using Param = std::tuple<Transport, int, int>;

// Test fixture.
class SendRecvTest : public BaseTest,
                     public ::testing::WithParamInterface<Param> {};

TEST_P(SendRecvTest, AllToAll) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    using buffer_ptr = std::unique_ptr<::gloo::transport::UnboundBuffer>;
    std::vector<int> input(contextSize);
    std::vector<int> output(contextSize);
    std::vector<buffer_ptr> inputBuffers(contextSize);
    std::vector<buffer_ptr> outputBuffers(contextSize);

    // Initialize
    for (auto i = 0; i < context->size; i++) {
      input[i] = context->rank;
      output[i] = -1;
      inputBuffers[i] =
          context->createUnboundBuffer(&input[i], sizeof(input[i]));
      outputBuffers[i] =
          context->createUnboundBuffer(&output[i], sizeof(output[i]));
    }

    // Send a message with the local rank to every other rank
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      inputBuffers[i]->send(i, context->rank);
    }

    // Receive message from every other rank
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      outputBuffers[i]->recv(i, i);
    }

    // Wait for send and recv to complete
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      inputBuffers[i]->waitSend();
      outputBuffers[i]->waitRecv();
    }

    // Verify output
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      ASSERT_EQ(i, output[i]) << "Mismatch at index " << i;
    }
  });
}

TEST_P(SendRecvTest, AllToAllOffset) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    auto elementSize = sizeof(int);
    std::vector<int> input(contextSize);
    std::vector<int> output(contextSize);
    auto inputBuffer =
        context->createUnboundBuffer(input.data(), input.size() * elementSize);
    auto outputBuffer = context->createUnboundBuffer(
        output.data(), output.size() * elementSize);

    // Initialize
    for (auto i = 0; i < context->size; i++) {
      input[i] = i;
      output[i] = -1;
    }

    // Send a message with the local rank to every other rank
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      inputBuffer->send(i, 0, context->rank * elementSize, elementSize);
    }

    // Receive message from every other rank
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      outputBuffer->recv(i, 0, i * elementSize, elementSize);
    }

    // Wait for send and recv to complete
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      inputBuffer->waitSend();
      outputBuffer->waitRecv();
    }

    // Verify output
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      ASSERT_EQ(i, output[i]) << "Mismatch at index " << i;
    }
  });
}

TEST_P(SendRecvTest, AllToAllEmptyThenNonEmpty) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    auto elementSize = sizeof(int);
    std::vector<int> input(contextSize);
    std::vector<int> output(contextSize);
    using buffer_ptr = std::unique_ptr<::gloo::transport::UnboundBuffer>;
    std::vector<buffer_ptr> emptyInputBuffers(contextSize);
    std::vector<buffer_ptr> emptyOutputBuffers(contextSize);
    std::vector<buffer_ptr> nonEmptyInputBuffers(contextSize);
    std::vector<buffer_ptr> nonEmptyOutputBuffers(contextSize);

    // Initialize
    for (auto i = 0; i < context->size; i++) {
      input[i] = context->rank;
      output[i] = -1;
      emptyInputBuffers[i] = context->createUnboundBuffer(nullptr, 0);
      emptyOutputBuffers[i] = context->createUnboundBuffer(nullptr, 0);
      nonEmptyInputBuffers[i] =
          context->createUnboundBuffer(&input[i], elementSize);
      nonEmptyOutputBuffers[i] =
          context->createUnboundBuffer(&output[i], elementSize);
    }

    // Kick off all sends and receives
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      emptyOutputBuffers[i]->recv(i, i);
      emptyInputBuffers[i]->send(i, context->rank);
      nonEmptyOutputBuffers[i]->recv(i, i);
      nonEmptyInputBuffers[i]->send(i, context->rank);
    }

    // Wait for send and recv to complete
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      emptyInputBuffers[i]->waitSend();
      emptyOutputBuffers[i]->waitRecv();
      nonEmptyInputBuffers[i]->waitSend();
      nonEmptyOutputBuffers[i]->waitRecv();
    }

    // Verify output
    for (auto i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      ASSERT_EQ(i, output[i]) << "Mismatch at index " << i;
    }
  });
}

TEST_P(SendRecvTest, RecvFromAny) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    constexpr uint64_t slot = 0x1337;
    if (context->rank == 0) {
      std::unordered_set<int> outputData;
      std::unordered_set<int> outputRanks;
      int tmp;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));

      // Compile vector of ranks to receive from
      std::vector<int> ranks;
      for (auto i = 1; i < context->size; i++) {
        ranks.push_back(i);
      }

      // Receive from N-1 peers
      for (auto i = 1; i < context->size; i++) {
        int srcRank = -1;
        buf->recv(ranks, slot);
        buf->waitRecv(&srcRank);
        outputData.insert(tmp);
        outputRanks.insert(srcRank);
      }

      // Verify result
      for (auto i = 1; i < context->size; i++) {
        ASSERT_EQ(1, outputData.count(i)) << "Missing output " << i;
        ASSERT_EQ(1, outputRanks.count(i)) << "Missing rank " << i;
      }
    } else {
      // Send to rank 0
      int tmp = context->rank;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));
      buf->send(0, slot);
      buf->waitSend();
    }
  });
}

TEST_P(SendRecvTest, AbortRecv) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  constexpr uint64_t slot = 0x1337;
  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    // Test that an unbounded buffer should be able to abort its own waitRecv.
    if (context->rank == 0) {
      bool recvCompleted;
      int srcRank = -1;
      int tmp;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));

      std::thread waitRecvThreadAbort(
          [&]() { recvCompleted = buf->waitRecv(&srcRank); });
      buf->abortWaitRecv();
      waitRecvThreadAbort.join();
      ASSERT_FALSE(recvCompleted);

      // Future waitRecvs should not be aborted if we received one abortWaitRecv
      // previously
      std::thread waitRecvThread([&]() {
        std::vector<int> ranks = {1};
        buf->recv(ranks, slot);
        recvCompleted = buf->waitRecv(&srcRank);
      });
      waitRecvThread.join();
      ASSERT_TRUE(recvCompleted);
    } else if (context->rank == 1) {
      // Send to rank 0
      int tmp = context->rank;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));
      buf->send(0, slot);
      buf->waitSend();
    }
  });
}

TEST_P(SendRecvTest, AbortSend) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());
  constexpr uint64_t slot = 0x1337;
  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    // Test that an unbounded buffer should be able to abort its own waitSend
    bool sendCompleted;
    if (context->rank == 0) {
      int tmp;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));

      std::thread waitSendThreadAbort(
          [&]() { sendCompleted = buf->waitSend(); });
      buf->abortWaitSend();
      waitSendThreadAbort.join();
      ASSERT_FALSE(sendCompleted);

      // Future wait sends should not be aborted if we received one abortSend
      // previously
      std::thread waitSendThread([&]() {
        // Send to rank 1
        buf->send(1, slot);
        sendCompleted = buf->waitSend();
      });
      waitSendThread.join();
      ASSERT_TRUE(sendCompleted);
    } else if (context->rank == 1) {
      // Receive from rank 0
      int tmp = context->rank;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));
      int srcRank = 0;
      buf->recv(srcRank, slot);
      buf->waitRecv();
    }
  });
}

TEST_P(SendRecvTest, RecvFromAnyOffset) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    auto elementSize = sizeof(int);
    constexpr uint64_t slot = 0x1337;
    if (context->rank == 0) {
      std::unordered_set<int> outputData;
      std::unordered_set<int> outputRanks;
      std::array<int, 2> tmp;
      auto buf =
          context->createUnboundBuffer(tmp.data(), tmp.size() * elementSize);

      // Compile vector of ranks to receive from
      std::vector<int> ranks;
      for (auto i = 1; i < context->size; i++) {
        ranks.push_back(i);
      }

      // Receive from N-1 peers
      for (auto i = 1; i < context->size; i++) {
        int srcRank = -1;
        buf->recv(ranks, slot, (i % tmp.size()) * elementSize, elementSize);
        buf->waitRecv(&srcRank);
        outputData.insert(tmp[i % tmp.size()]);
        outputRanks.insert(srcRank);
      }

      // Verify result
      for (auto i = 1; i < context->size; i++) {
        ASSERT_EQ(1, outputData.count(i)) << "Missing output " << i;
        ASSERT_EQ(1, outputRanks.count(i)) << "Missing rank " << i;
      }
    } else {
      // Send to rank 0
      int tmp = context->rank;
      auto buf = context->createUnboundBuffer(&tmp, sizeof(tmp));
      buf->send(0, slot);
      buf->waitSend();
    }
  });
}

TEST_P(SendRecvTest, RecvFromAnyPipeline) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    constexpr uint64_t slot = 0x1337;

    if (context->rank == 0) {
      std::vector<int> output;
      std::array<int, 2> tmp;
      auto buf0 = context->createUnboundBuffer(&tmp[0], sizeof(tmp[0]));
      auto buf1 = context->createUnboundBuffer(&tmp[1], sizeof(tmp[1]));

      // Compile vector of ranks to receive from
      std::vector<int> ranks;
      for (auto i = 0; i < context->size; i++) {
        if (i == context->rank) {
          continue;
        }
        ranks.push_back(i);
      }

      // Receive twice per peer
      for (auto i = 0; i < context->size - 1; i++) {
        buf0->recv(ranks, slot);
        buf1->recv(ranks, slot);
        buf0->waitRecv();
        buf1->waitRecv();
        output.push_back(tmp[0]);
        output.push_back(tmp[1]);
      }

      // Verify output
      std::sort(output.begin(), output.end());
      for (auto i = 1; i < context->size; i++) {
        ASSERT_EQ(i, output[(i - 1) * 2 + 0]) << "Mismatch at " << i;
        ASSERT_EQ(i, output[(i - 1) * 2 + 1]) << "Mismatch at " << i;
      }
    } else {
      // Send twice to rank 0 on the same slot
      std::array<int, 2> tmp;
      tmp[0] = context->rank;
      tmp[1] = context->rank;
      auto buf0 = context->createUnboundBuffer(&tmp[0], sizeof(tmp[0]));
      auto buf1 = context->createUnboundBuffer(&tmp[1], sizeof(tmp[1]));
      buf0->send(0, slot);
      buf1->send(0, slot);
      buf0->waitSend();
      buf1->waitSend();
    }
  });
}

// This test stresses the pattern that is commonly used for some sort
// of RPC on top of send/recv primitives. The server executes an
// indirect recv (that can be fulfilled any some subset of peers) to
// receive the message size and rank of a client, followed by an
// immediate recv from that rank to get the actual message.
TEST_P(SendRecvTest, RecvFromAnyRPC) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    constexpr uint64_t slot = 0x1337;
    constexpr auto niters = 10;
    size_t tmp0;
    size_t tmp1;
    auto buf0 = context->createUnboundBuffer(&tmp0, sizeof(tmp0));
    auto buf1 = context->createUnboundBuffer(&tmp1, sizeof(tmp1));

    if (context->rank == 0) {
      // Keep number of recvs per rank
      std::unordered_map<int, int> counts;
      // Compile vector of ranks to receive from
      std::vector<int> allRanks;
      for (auto i = 0; i < context->size; i++) {
        if (i == context->rank) {
          continue;
        }
        allRanks.push_back(i);
      }
      // Receive twice from every peer, niters times
      for (auto i = 0; i < (niters * (context->size - 1)); i++) {
        int rank;
        // Receive from any peer first
        buf0->recv(allRanks, slot);
        buf0->waitRecv(&rank);
        counts[rank]++;
        // Then receive from the same peer again
        buf1->recv(rank, slot);
        buf1->waitRecv();
      }
      // Verify result
      for (auto i = 0; i < context->size; i++) {
        if (i == context->rank) {
          continue;
        }
        ASSERT_EQ(niters, counts[i]) << "recv mismatch for rank " << i;
      }
    } else {
      for (auto i = 0; i < niters; i++) {
        buf0->send(0, slot);
        buf0->waitSend();
        buf1->send(0, slot);
        buf1->waitSend();
      }
    }
  });
}

TEST_P(SendRecvTest, RecvFromAnyRPCEmptyThenNonEmpty) {
  const auto transport = std::get<0>(GetParam());
  const auto contextSize = std::get<1>(GetParam());

  spawn(transport, contextSize, [&](std::shared_ptr<Context> context) {
    constexpr uint64_t slot = 0x1337;
    constexpr auto niters = 10;
    size_t tmp0;
    size_t tmp1;
    auto buf0 = context->createUnboundBuffer(&tmp0, sizeof(tmp0));
    auto buf1 = context->createUnboundBuffer(&tmp1, sizeof(tmp1));

    if (context->rank == 0) {
      // Compile vector of ranks to receive from
      std::vector<int> allRanks;
      for (auto i = 0; i < context->size; i++) {
        if (i == context->rank) {
          continue;
        }
        allRanks.push_back(i);
      }
      // Receive twice from every peer, niters times
      for (auto i = 0; i < (niters * (context->size - 1)); i++) {
        int rank;
        // Receive from any peer first (0 bytes)
        tmp0 = 0xdeadbeef;
        buf0->recv(allRanks, slot, /* offset= */ 0, /* nbytes= */ 0);
        buf0->waitRecv(&rank);
        ASSERT_EQ(tmp0, 0xdeadbeef);
        // Then receive from the same peer again
        tmp1 = 0xdeadbeef;
        buf1->recv(rank, slot);
        buf1->waitRecv();
        GLOO_ENFORCE_EQ(tmp1, rank);
      }
    } else {
      for (auto i = 0; i < niters; i++) {
        tmp0 = context->rank;
        buf0->send(0, slot, /* offset= */ 0, /* nbytes= */ 0);
        buf0->waitSend();
        tmp1 = context->rank;
        buf1->send(0, slot);
        buf1->waitSend();
      }
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    SendRecvDefault,
    SendRecvTest,
    ::testing::Combine(
        ::testing::ValuesIn(kTransportsForFunctionAlgorithms),
        ::testing::Values(2, 3, 4, 5, 6, 7, 8),
        ::testing::Values(1)));

} // namespace
} // namespace test
} // namespace gloo
