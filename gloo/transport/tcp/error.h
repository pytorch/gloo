/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gloo/transport/tcp/address.h>
#include <string>

namespace gloo {
namespace transport {
namespace tcp {

class Error {
 public:
  // Constant instance that indicates success.
  static const Error kSuccess;

  /* implicit */ Error() : valid_(false) {}

  virtual ~Error() = default;

  // Don't allow Error to be copied or moved to avoid losing the error message.
  Error(const Error&) = delete;
  Error& operator=(const Error&) = delete;

  // Converting to boolean means checking if there is an error. This
  // means we don't need to use an `std::optional` and allows for a
  // snippet like the following:
  //
  //   if (error) {
  //     // Deal with it.
  //   }
  //
  operator bool() const {
    return valid_;
  }

  // Returns an explanatory string.
  // Like `std::exception` but returns a `std::string`.
  virtual std::string what() const;

 protected:
  explicit Error(bool valid) : valid_(valid) {}

 private:
  const bool valid_;
};

class SystemError : public Error {
 public:
  explicit SystemError(const char* syscall, int error, Address remote)
      : Error(true),
        syscall_(syscall),
        error_(error),
        remote_(std::move(remote)) {}

  std::string what() const override;

 private:
  const char* syscall_;
  const int error_;
  const Address remote_;
};

class ShortReadError : public Error {
 public:
  ShortReadError(ssize_t expected, ssize_t actual, Address remote)
      : Error(true),
        expected_(expected),
        actual_(actual),
        remote_(std::move(remote)) {}

  std::string what() const override;

 private:
  const ssize_t expected_;
  const ssize_t actual_;
  const Address remote_;
};

class ShortWriteError : public Error {
 public:
  ShortWriteError(ssize_t expected, ssize_t actual, Address remote)
      : Error(true),
        expected_(expected),
        actual_(actual),
        remote_(std::move(remote)) {}

  std::string what() const override;

 private:
  const ssize_t expected_;
  const ssize_t actual_;
  const Address remote_;
};

class TimeoutError : public Error {
 public:
  explicit TimeoutError(std::string msg) : Error(true), msg_(std::move(msg)) {}

  std::string what() const override;

 private:
  const std::string msg_;
};

class LoopError : public Error {
 public:
  explicit LoopError(std::string msg) : Error(true), msg_(std::move(msg)) {}

  std::string what() const override;

 private:
  const std::string msg_;
};

} // namespace tcp
} // namespace transport
} // namespace gloo
