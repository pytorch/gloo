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
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <cstdio>

namespace {

static void segfault_handler(int sig) {
  void* array[30];
  int size = backtrace(array, 30);
  fprintf(stderr, "[DIAG] Signal %d caught, backtrace:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  _exit(128 + sig);
}

struct Initializer {
  Initializer() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, segfault_handler);
  }
};
Initializer initializer;
} // namespace
#endif

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
