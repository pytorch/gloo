/**
 * Copyright (c) 2024-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/test/base_test.h"

#include "gloo/common/logging.h"

#if GLOO_HAVE_TRANSPORT_TCP

namespace gloo {
namespace test {
namespace {

// Test parameterization (transport).
using Param = Transport;

// Test fixture.
class PairTest : public BaseTest,
                 public ::testing::WithParamInterface<Param> {};

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
