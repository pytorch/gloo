/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>

#ifdef _WIN32
#include "gloo/common/win.h" // @manual
#else
#include <sys/socket.h>
#endif

#include "gloo/transport/address.h"

namespace gloo {
namespace transport {
namespace tcp {

using sequence_number_t = ssize_t;

class Address : public ::gloo::transport::Address {
 public:
  static constexpr sequence_number_t kSequenceNumberUnset = -1;

  Address() {}

  explicit Address(struct sockaddr_storage ss, sequence_number_t seq = -1);

  explicit Address(const struct sockaddr* addr, size_t addrlen);

  explicit Address(const std::vector<char>&);

  explicit Address(
      const std::string& ip,
      uint16_t port,
      sequence_number_t seq = -1);

  Address(const Address& other);

  Address& operator=(Address&& other);
  Address& operator=(const Address& other);

  virtual std::vector<char> bytes() const override;

  virtual std::string str() const override;

  const struct sockaddr_storage& getSockaddr() const {
    return impl_.ss;
  }

  sequence_number_t getSeq() const {
    return impl_.seq;
  }

  static Address fromSockName(int fd);

  static Address fromPeerName(int fd);

 protected:
  // Encapsulate fields such that it is trivially copyable. This class
  // is not trivially copyable itself.
  struct Impl {
    // IP address of the listening socket.
    struct sockaddr_storage ss;

    // Sequence number of this address.
    // If this is equal to -1, the address is assumed to
    // represent the listening socket of a device. The sequence number
    // must be set before it can be used by a pair.
    sequence_number_t seq{kSequenceNumberUnset};
  };

#if __GNUG__ && __GNUC__ < 5
  static_assert(__has_trivial_copy(Impl), "!");
#else
  static_assert(std::is_trivially_copyable<Impl>::value, "!");
#endif
  static_assert(sizeof(Impl) <= kMaxByteSize, "!");

  Impl impl_;
  mutable std::mutex m_;
};

} // namespace tcp
} // namespace transport
} // namespace gloo
