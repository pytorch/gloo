/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/config.h"
#if GLOO_HAVE_TRANSPORT_TCP_TLS

#include <gmock/gmock.h>

#include "gloo/test/multiproc_test.h"
#include "gloo/test/openssl_utils.h"

namespace gloo {
namespace test {
namespace {

const char* kDefaultDevice = "localhost";

class TlsTcpTest : public BaseTest {};

TEST_F(TlsTcpTest, CreateDeviceWithAllEmptyFilePaths) {
  bool exception_thrown = false;
  try {
    ::gloo::rendezvous::HashStore store;
    auto device = ::gloo::transport::tcp::tls::CreateDevice(
        kDefaultDevice, "", "", "", "");
    auto context = device->createContext(0, 1);
  } catch (::gloo::EnforceNotMet e) {
    exception_thrown = true;
    ASSERT_THAT(
        e.what(),
        ::testing::ContainsRegex(
            "Private key and certificate location must be specified"));
  }
  ASSERT_TRUE(exception_thrown);
}

TEST_F(TlsTcpTest, CreateDeviceWithCAEmptyFilePaths) {
  bool exception_thrown = false;
  try {
    ::gloo::rendezvous::HashStore store;
    auto device = ::gloo::transport::tcp::tls::CreateDevice(
        kDefaultDevice, pkey_file, cert_file, "", "");
    auto context = device->createContext(0, 1);
  } catch (::gloo::EnforceNotMet e) {
    exception_thrown = true;
    ASSERT_THAT(
        e.what(),
        ::testing::ContainsRegex("CAfile or CApath must be specified"));
  }
  ASSERT_TRUE(exception_thrown);
}

TEST_F(TlsTcpTest, CreateDeviceWithUnknownCA) {
  auto device = ::gloo::transport::tcp::tls::CreateDevice(
      kDefaultDevice, pkey_file, cert_file, cert_file, "");
  auto context = device->createContext(0, 2);
  auto& pair0 = context->createPair(0);
  auto addrBytes0 = pair0->address().bytes();
  auto& pair1 = context->createPair(1);
  auto addrBytes1 = pair1->address().bytes();

  bool exception_thrown = false;
  spawnThreads(2, [&](int rank) {
    try {
      if (rank == 0) {
        pair0->connect(addrBytes1);
      } else {
        pair1->connect(addrBytes0);
      }
    } catch (::gloo::IoException e) {
      exception_thrown = true;
      ASSERT_THAT(e.what(), ::testing::ContainsRegex("[unknown ca|Connect timeout|Connection refused]"));
    } catch (::gloo::EnforceNotMet e) {
      exception_thrown = true;
      ASSERT_THAT(
          e.what(), ::testing::ContainsRegex("handshake was not succeeded"));
    }
  });

  ASSERT_TRUE(exception_thrown);
}

} // namespace
} // namespace test
} // namespace gloo
#endif
