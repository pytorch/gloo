/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/common/logging.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>

namespace gloo {

// Initialize log level from environment variable, and return static value at
// each inquiry.
LogLevel logLevel() {
  // Global log level. Initialized once.
  static const LogLevel log_level = []() {
    const char* level = std::getenv("GLOO_LOG_LEVEL");
    // Defaults to WARN.
    if (level == nullptr) {
      return LogLevel::WARN;
    }

    if (std::strcmp(level, "DEBUG") == 0) {
      return LogLevel::DEBUG;
    } else if (std::strcmp(level, "INFO") == 0) {
      return LogLevel::INFO;
    } else if (std::strcmp(level, "WARN") == 0) {
      return LogLevel::WARN;
    } else {
      return LogLevel::ERROR;
    }
  }();
  return log_level;
}

EnforceNotMet::EnforceNotMet(
    const char* file,
    const int line,
    const char* condition,
    const std::string& msg)
    : msg_stack_{MakeString(
          "[enforce fail at ",
          file,
          ":",
          line,
          "] ",
          condition,
          ". ",
          msg)} {
  full_msg_ = this->msg();
}

std::string EnforceNotMet::msg() const {
  return std::accumulate(msg_stack_.begin(), msg_stack_.end(), std::string(""));
}

const char* EnforceNotMet::what() const noexcept {
  return full_msg_.c_str();
}

} // namespace gloo
