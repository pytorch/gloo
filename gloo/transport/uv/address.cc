/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/uv/address.h>

#include <string.h>

#include <uv.h>

#include <gloo/common/enforce.h>

namespace gloo {
namespace transport {
namespace uv {

Address::Address(struct sockaddr_storage ss, sequence_type seq) {
  impl_.ss = std::move(ss);
  impl_.seq = seq;
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

std::string Address::str() const {
  char str[INET6_ADDRSTRLEN + 128];
  int port = 0;

  str[0] = '[';
  if (impl_.ss.ss_family == AF_INET) {
    auto in = (struct sockaddr_in*)&impl_.ss;
    uv_ip4_name(in, str + 1, sizeof(str) - 1);
    port = in->sin_port;
  } else if (impl_.ss.ss_family == AF_INET6) {
    auto in6 = (struct sockaddr_in6*)&impl_.ss;
    uv_ip6_name(in6, str + 1, sizeof(str) - 1);
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
  if (impl_.seq != SIZE_MAX) {
    len += snprintf(str + len, sizeof(str) - len, "$%d", impl_.seq);
  }

  return str;
}

} // namespace uv
} // namespace transport
} // namespace gloo
