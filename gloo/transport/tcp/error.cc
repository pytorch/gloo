/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/tcp/error.h>

#include <cstring>
#include <sstream>

namespace gloo {
namespace transport {
namespace tcp {

const Error Error::kSuccess = Error();

std::string Error::what() const {
  return "no error";
}

std::string SystemError::what() const {
  std::ostringstream ss;
  ss << syscall_ << ": " << strerror(error_) << ", remote=" << remote_.str();
  return ss.str();
}

std::string ShortReadError::what() const {
  std::ostringstream ss;
  ss << "short read: got " << actual_ << " bytes while expecting to read "
     << expected_ << " bytes, remote=" << remote_.str();
  return ss.str();
}

std::string ShortWriteError::what() const {
  std::ostringstream ss;
  ss << "short write: wrote " << actual_ << " bytes while expecting to write "
     << expected_ << " bytes, remote=" << remote_.str();
  return ss.str();
}

std::string TimeoutError::what() const {
  return msg_;
}

std::string LoopError::what() const {
  return msg_;
}

} // namespace tcp
} // namespace transport
} // namespace gloo
