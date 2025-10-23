/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/tcp/address.h"

#include <arpa/inet.h>
#include <string.h>
#include <memory>
#include <mutex>

#include "gloo/common/enforce.h"

namespace gloo {
namespace transport {
namespace tcp {

Address::Address(struct sockaddr_storage ss, sequence_number_t seq) {
  impl_.ss = std::move(ss);
  impl_.seq = seq;
}

Address::Address(const struct sockaddr* addr, size_t addrlen) {
  memcpy(&impl_.ss, addr, addrlen);
}

Address::Address(const std::string& ip, uint16_t port, sequence_number_t seq) {
  if (ip.empty()) {
    throw std::invalid_argument("Invalid IP address");
  }
  sockaddr_in* addr4 = reinterpret_cast<sockaddr_in*>(&impl_.ss);
  sockaddr_in6* addr6 = reinterpret_cast<sockaddr_in6*>(&impl_.ss);
  // Check if the IP address is an IPv4 or IPv6 address
  if (inet_pton(AF_INET, ip.c_str(), &addr4->sin_addr) == 1) {
    // IPv4 address
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
  } else if (inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr) == 1) {
    // IPv6 address
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
  } else {
    throw std::invalid_argument("Invalid IP address");
  }

  // Store sequence number
  impl_.seq = seq;
}

Address& Address::operator=(Address&& other) {
  std::lock_guard<std::mutex> lock(m_);
  impl_.ss = std::move(other.impl_.ss);
  impl_.seq = other.impl_.seq;
  return *this;
}

Address& Address::operator=(const Address& other) {
  std::lock_guard<std::mutex> lock(m_);
  impl_.ss = other.impl_.ss;
  impl_.seq = other.impl_.seq;
  return *this;
}

Address::Address(const std::vector<char>& bytes) {
  GLOO_ENFORCE_EQ(sizeof(impl_), bytes.size());
  memcpy(&impl_, bytes.data(), sizeof(impl_));
}

Address::Address(const Address& other)
    : Address(other.impl_.ss, other.impl_.seq) {}

std::vector<char> Address::bytes() const {
  std::lock_guard<std::mutex> lock(m_);
  std::vector<char> bytes(sizeof(impl_));
  memcpy(bytes.data(), &impl_, sizeof(impl_));
  return bytes;
}

std::string Address::str() const {
  char str[INET6_ADDRSTRLEN + 128];
  int port = 0;

  str[0] = '[';
  if (impl_.ss.ss_family == AF_INET) {
    struct sockaddr_in* in = (struct sockaddr_in*)&impl_.ss;
    inet_ntop(AF_INET, &in->sin_addr, str + 1, sizeof(str) - 1);
    port = in->sin_port;
  } else if (impl_.ss.ss_family == AF_INET6) {
    struct sockaddr_in6* in6 = (struct sockaddr_in6*)&impl_.ss;
    inet_ntop(AF_INET6, &in6->sin6_addr, str + 1, sizeof(str) - 1);
    port = in6->sin6_port;
  } else {
    snprintf(str + 1, sizeof(str) - 1, "none");
  }

  size_t len = strlen(str);
  if (port > 0) {
    len += snprintf(str + len, sizeof(str) - len, "]:%d", port);
  } else {
    len += snprintf(str + len, sizeof(str) - len, "]");
  }

  // Append sequence number if one is set.
  if (impl_.seq != kSequenceNumberUnset) {
    len += snprintf(str + len, sizeof(str) - len, "$%ld", impl_.seq);
  }

  return str;
}

Address Address::fromSockName(int fd) {
  struct sockaddr_storage ss;
  socklen_t addrlen = sizeof(ss);
  int rv;

  rv = getsockname(fd, (struct sockaddr*)&ss, &addrlen);
  GLOO_ENFORCE_NE(rv, -1, "getsockname: ", strerror(errno));
  return Address(ss);
}

Address Address::fromPeerName(int fd) {
  struct sockaddr_storage ss;
  socklen_t addrlen = sizeof(ss);
  int rv;

  rv = getpeername(fd, (struct sockaddr*)&ss, &addrlen);
  GLOO_ENFORCE_NE(rv, -1, "getpeername: ", strerror(errno));
  return Address(ss);
}

} // namespace tcp
} // namespace transport
} // namespace gloo
