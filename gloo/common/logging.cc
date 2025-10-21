/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/common/logging.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace gloo {

// Initialize log level from environment variable, and return static value at
// each inquiry.
LogLevel logLevel() {
  // Global log level. Initialized once.
  static LogLevel log_level = LogLevel::UNSET;
  if (log_level != LogLevel::UNSET) {
    return log_level;
  }

  const char* level = getenv("GLOO_LOG_LEVEL");
  // Defaults to WARN.
  if (level == nullptr) {
    log_level = LogLevel::WARN;
    return log_level;
  }

  if (std::strcmp(level, "DEBUG") == 0) {
    log_level = LogLevel::DEBUG;
  } else if (std::strcmp(level, "INFO") == 0) {
    log_level = LogLevel::INFO;
  } else if (std::strcmp(level, "WARN") == 0) {
    log_level = LogLevel::WARN;
  } else {
    log_level = LogLevel::ERROR;
  }
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
