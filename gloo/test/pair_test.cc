/**
 * Copyright (c) 2024-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/test/base_test.h"

#include <string>

#include "gloo/common/logging.h"

#if GLOO_HAVE_TRANSPORT_TCP

#include "gloo/transport/tcp/buffer.h"

namespace gloo {
namespace test {
namespace {

// Model an operation paused after its preamble, without timing socket I/O.
class PendingSendBuffer : public transport::tcp::Buffer {
 public:
  PendingSendBuffer(
      transport::tcp::Pair* pair,
      int slot,
      void* ptr,
      size_t size)
      : Buffer(pair, slot, ptr, size) {
    sendPending_ = 1;
  }
};

class PendingBufferPair : public transport::tcp::Pair {
 public:
  explicit PendingBufferPair(transport::tcp::Device* device)
      : Pair(nullptr, device, 1, std::chrono::seconds(1)) {}

  void setPending(transport::Buffer* buffer, bool send) {
    transport::tcp::Op op;
    op.buf = static_cast<transport::tcp::Buffer*>(buffer);
    if (send) {
      tx_.push_back(op);
    } else {
      rx_ = op;
    }
  }

  bool hasPendingPointers() const {
    return rx_.buf != nullptr || !tx_.empty();
  }

  bool failed() const {
    return ex_ != nullptr;
  }

  bool registered(int slot) {
    return getBuffer(slot) != nullptr;
  }
};

TEST(PairBufferTest, DestroyPendingBuffer) {
  transport::tcp::attr attributes;
  attributes.hostname = "127.0.0.1";
  auto device = std::dynamic_pointer_cast<transport::tcp::Device>(
      transport::tcp::CreateDevice(attributes));
  for (bool send : {false, true}) {
    PendingBufferPair pair(device.get());
    char data = 0;
    auto buffer = send ? pair.createSendBuffer(0, &data, sizeof(data))
                       : pair.createRecvBuffer(0, &data, sizeof(data));
    auto other =
        std::make_unique<PendingSendBuffer>(&pair, 1, &data, sizeof(data));
    pair.setPending(buffer.get(), send);
    pair.setPending(other.get(), true);
    buffer.reset();
    EXPECT_TRUE(pair.failed());
    EXPECT_FALSE(pair.hasPendingPointers());
    EXPECT_THROW(other->waitSend(), IoException);
  }
}

TEST(PairBufferTest, DestroyIdleSendBufferPreservesReceiveSlot) {
  transport::tcp::attr attributes;
  attributes.hostname = "127.0.0.1";
  auto device = std::dynamic_pointer_cast<transport::tcp::Device>(
      transport::tcp::CreateDevice(attributes));
  PendingBufferPair pair(device.get());
  char data = 0;
  auto receive = pair.createRecvBuffer(0, &data, sizeof(data));
  auto send = pair.createSendBuffer(0, &data, sizeof(data));
  send.reset();
  EXPECT_TRUE(pair.registered(0));
  EXPECT_FALSE(pair.failed());
  receive.reset();
  EXPECT_FALSE(pair.registered(0));
  EXPECT_FALSE(pair.failed());
}

// Test parameterization (transport).
using Param = Transport;

// Test fixture.
class PairTest : public BaseTest,
                 public ::testing::WithParamInterface<Param> {};

TEST_P(PairTest, ReportsPeerRank) {
  const auto transport = GetParam();

  spawn(transport, 2, [&](std::shared_ptr<Context> context) {
    const int peer = (context->rank + 1) % 2;
    auto& pair = context->getPair(peer);
    EXPECT_EQ(peer, pair->getPeerRank());
    EXPECT_NE(
        pair->peerDescription().find("rank " + std::to_string(peer)),
        std::string::npos);
  });
}

// Regression test for the size_t overflow in tcp::Pair::prepareRead.
//
// A SEND_BUFFER preamble carries `roffset` and `length` as size_t values read
// directly off the socket. The receive bounds check used to be
// `roffset + length <= size_`, where the addition wraps modulo 2^64. A
// malicious peer could pick e.g. roffset = (size_t)-8 and length = 8 so the sum
// wraps to 0 and passes the check, while the payload gets written at
// `ptr_ + roffset` -- an out-of-bounds write relative to the registered buffer.
//
// The receiver must reject such a preamble instead of performing the write.
TEST_P(PairTest, RecvBufferRejectsRoffsetLengthOverflow) {
  const auto transport = GetParam();

  spawn(transport, 2, [&](std::shared_ptr<Context> context) {
    constexpr size_t kSize = 64;
    std::vector<char> data(kSize);

    const int peer = (context->rank + 1) % 2;
    auto& pair = context->getPair(peer);

    // Use synchronous mode so reads happen on this thread; the resulting
    // exception can then be observed directly. In async mode the read runs on
    // the device loop thread where it cannot be caught by the test.
    pair->setSync(true, false);

    if (context->rank == 0) {
      // Receiver: register a buffer and expect the malicious send to be
      // rejected by the overflow-safe bounds check.
      auto recvBuffer = pair->createRecvBuffer(0, data.data(), data.size());
      EXPECT_THROW(recvBuffer->waitRecv(), ::gloo::EnforceNotMet);
    } else {
      // Sender: emulate a malicious peer. The sender-side check only validates
      // `offset + length` against the local buffer, so `roffset` (the remote
      // offset) passes through onto the wire unchecked.
      auto sendBuffer = pair->createSendBuffer(0, data.data(), data.size());
      const size_t length = 8;
      // roffset + length wraps to 0: (2^64 - 8) + 8 == 0, which is <= size_.
      const size_t roffset = ~static_cast<size_t>(0) - (length - 1);
      sendBuffer->send(/*offset=*/0, length, roffset);
      sendBuffer->waitSend();
    }
  });
}

// Positive control: a valid (non-overflowing) roffset must still be honored,
// i.e. the payload lands at `ptr_ + roffset` in the receive buffer. This guards
// against the bounds check being made overly strict.
TEST_P(PairTest, RecvBufferHonorsValidRoffset) {
  const auto transport = GetParam();

  spawn(transport, 2, [&](std::shared_ptr<Context> context) {
    constexpr size_t kSize = 64;
    constexpr size_t kLength = 8;
    constexpr size_t kRoffset = 16;
    std::vector<char> data(kSize, 0);

    const int peer = (context->rank + 1) % 2;
    auto& pair = context->getPair(peer);
    pair->setSync(true, false);

    if (context->rank == 0) {
      auto recvBuffer = pair->createRecvBuffer(0, data.data(), data.size());
      recvBuffer->waitRecv();

      // Payload must land exactly in [kRoffset, kRoffset + kLength).
      std::vector<char> expected(kSize, 0);
      for (size_t i = 0; i < kLength; i++) {
        expected[kRoffset + i] = static_cast<char>(i + 1);
      }
      EXPECT_EQ(data, expected);
    } else {
      for (size_t i = 0; i < kLength; i++) {
        data[i] = static_cast<char>(i + 1);
      }
      auto sendBuffer = pair->createSendBuffer(0, data.data(), data.size());
      sendBuffer->send(/*offset=*/0, kLength, kRoffset);
      sendBuffer->waitSend();
    }
  });
}

INSTANTIATE_TEST_CASE_P(
    PairTests,
    PairTest,
    ::testing::Values(
        Transport::TCP
#if GLOO_HAVE_TRANSPORT_TCP_TLS
        ,
        Transport::TCP_TLS
#endif
        ));

} // namespace
} // namespace test
} // namespace gloo

#endif // GLOO_HAVE_TRANSPORT_TCP
