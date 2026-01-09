/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifdef _WIN32
#include "gloo/common/win.h" // @manual
#else
#include <sys/socket.h>
#endif

#include <mutex>

#include <gloo/transport/address.h>

namespace gloo {
namespace transport {
namespace uv {

class Address : public ::gloo::transport::Address {
 public:
  using sequence_type = int;

  Address() = default;

  Address(struct sockaddr_storage ss, sequence_type seq = -1);

  explicit Address(const std::vector<char>&);

  Address& operator=(Address&& other);
  Address& operator=(const Address& other);
  Address(const Address& other);

  std::vector<char> bytes() const override;

  std::string str() const override;

  const struct sockaddr_storage& getSockaddr() const {
    return impl_.ss;
  }

  sequence_type getSeq() const {
    return impl_.seq;
  }

  Address withSeq(sequence_type seq) const {
    return Address(impl_.ss, seq);
  }

 protected:
  // Encapsulate fields such that it is trivially copyable. This class
  // is not trivially copyable itself (because it is a subclass?).
  struct Impl {
    // IP address of the listening socket.
    struct sockaddr_storage ss;

    // Sequence number of this address.
    // If this is equal to -1, the address is assumed to
    // represent the listening socket of a device. The sequence number
    // must be set before it can be used by a pair.
    sequence_type seq = -1;
  };

  static_assert(std::is_trivially_copyable_v<Impl>, "!");

  Impl impl_;
  mutable std::mutex m_;
};

} // namespace uv
} // namespace transport
} // namespace gloo
