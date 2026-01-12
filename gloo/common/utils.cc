/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#endif

#ifdef __APPLE__
#include <pthread.h>
#endif

#include "gloo/common/utils.h"

namespace gloo {

constexpr int HOSTNAME_MAX_SIZE = 192;
constexpr int THREAD_NAME_MAX_SIZE = 15;

std::string getHostname() {
  // Get Hostname using syscall
  char hostname[HOSTNAME_MAX_SIZE]; // NOLINT
  int rv = gethostname(hostname, HOSTNAME_MAX_SIZE);
  if (rv != 0) {
    throw std::system_error(errno, std::system_category());
  }
  return std::string(hostname);
}

bool useRankAsSeqNumber() {
  const auto& res = getenv("GLOO_ENABLE_RANK_AS_SEQUENCE_NUMBER");
  return res != nullptr &&
      (std::string(res) == "True" || std::string(res) == "1");
}

bool isStoreExtendedApiEnabled() {
  const auto& res = std::getenv("GLOO_ENABLE_STORE_V2_API");
  return res != nullptr &&
      (std::string(res) == "True" || std::string(res) == "1");
}

bool disableConnectionRetries() {
  // use meyer singleton to only compute this exactly once.
  static bool disable = []() {
    const auto& res = std::getenv("GLOO_DISABLE_CONNECTION_RETRIES");
    return res != nullptr &&
        (std::string(res) == "True" || std::string(res) == "1");
  }();
  return disable;
}

void setThreadName(const std::string& name) {
  // Thread names are limited to 15 characters on Linux (plus null terminator)
  std::string truncatedName = name.substr(0, THREAD_NAME_MAX_SIZE);
#if defined(__linux__)
  pthread_setname_np(pthread_self(), truncatedName.c_str());
#elif defined(__APPLE__)
  pthread_setname_np(truncatedName.c_str());
#endif
  // On Windows and other platforms, this is a no-op
}

} // namespace gloo
