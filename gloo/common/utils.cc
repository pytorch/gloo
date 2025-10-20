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
#include <unistd.h>
#include <cstdlib>
#endif

#include "gloo/common/utils.h"

namespace gloo {

constexpr int HOSTNAME_MAX_SIZE = 192;

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

} // namespace gloo
