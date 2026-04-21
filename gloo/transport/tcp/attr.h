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

namespace gloo {
namespace transport {
namespace tcp {

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

  // Pre-bound socket file descriptor. When set to a valid fd (>= 0),
  // the Listener will adopt this socket instead of creating a new one
  // and binding again. This avoids EADDRINUSE races between the test
  // bind in address resolution and the actual listener bind.
  int ai_fd = -1;
};

} // namespace tcp
} // namespace transport
} // namespace gloo
