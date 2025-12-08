/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

// One-time init to use EPIPE errors instead of SIGPIPE
#ifndef _WIN32
#include <csignal>
namespace {
struct Initializer {
  Initializer() {
    signal(SIGPIPE, SIG_IGN);
  }
};
Initializer initializer;
} // namespace
#endif

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
