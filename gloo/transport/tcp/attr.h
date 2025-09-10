/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <sys/socket.h>
#include <unistd.h>
#include <memory>

namespace gloo {
namespace transport {
namespace tcp {

// RAII for fds so they'll close when all references to them are freed.
class FDHolder {
 public:
  explicit FDHolder(int fd) : fd_(fd) {}
  ~FDHolder() {
    close(fd_);
  }

  FDHolder(const FDHolder&) = delete;
  FDHolder& operator=(const FDHolder&) = delete;
  FDHolder(FDHolder&&) = delete;
  FDHolder& operator=(FDHolder&&) = delete;

 private:
  const int fd_;
};

struct attr {
  attr() {}
  /* implicit */ attr(const char* ptr) : hostname(ptr) {}

  std::string hostname;

  std::string iface;

  // The address family defaults to AF_UNSPEC such that getaddrinfo(3)
  // will try to find either IPv4 or IPv6 addresses.
  int ai_family = AF_UNSPEC;
  int ai_socktype;
  int ai_protocol;
  struct sockaddr_storage ai_addr;
  int ai_addrlen;

  std::shared_ptr<FDHolder> fd{nullptr};
};

} // namespace tcp
} // namespace transport
} // namespace gloo
