/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gloo/transport/remote_key.h>

namespace gloo {
namespace transport {

constexpr auto kUnsetTimeout = std::chrono::milliseconds(-1);

// The unbound buffer class represents a chunk of memory.
// It can either be used as a source for send operations or a
// destination for receive operations, or both. There should only be a
// single pending operation against an unbound buffer at any given
// time, or resulting behavior is undefined.
//
// It is called unbound to contrast with the bound buffers that have
// been available since the inception of Gloo. It is unbound in that
// it is not tied to a particular pair.
//
class UnboundBuffer {
 public:
  UnboundBuffer(void* ptr, size_t size) : ptr(ptr), size(size) {}
  virtual ~UnboundBuffer() = 0;

  void* const ptr;
  const size_t size;

  // If specified, the source of this recv is stored in the rank pointer.
  // Returns true if it completed, false if it was aborted.
  virtual bool waitRecv(int* rank, std::chrono::milliseconds timeout) = 0;

  // If specified, the destination of this send is stored in the rank pointer.
  // Returns true if it completed, false if it was aborted.
  // put/get are both considered sends since they're initiated by the sender so
  // you need to call waitSend for those operations.
  virtual bool waitSend(int* rank, std::chrono::milliseconds timeout) = 0;

  // Aborts a pending waitRecv call.
  virtual void abortWaitRecv() = 0;

  // Aborts a pending waitSend call.
  virtual void abortWaitSend() = 0;

  // Default overload.
  bool waitRecv() {
    return waitRecv(nullptr, kUnsetTimeout);
  }

  // Default overload.
  bool waitSend() {
    return waitSend(nullptr, kUnsetTimeout);
  }

  // Rank overload.
  bool waitRecv(int* rank) {
    return waitRecv(rank, kUnsetTimeout);
  }

  // Rank overload.
  bool waitSend(int* rank) {
    return waitSend(rank, kUnsetTimeout);
  }

  // Timeout overload.
  bool waitRecv(std::chrono::milliseconds timeout) {
    return waitRecv(nullptr, timeout);
  }

  // Timeout overload.
  bool waitSend(std::chrono::milliseconds timeout) {
    return waitSend(nullptr, timeout);
  }

  // Deadline overload.
  template <typename clock>
  bool waitRecv(std::chrono::time_point<clock> deadline) {
    return waitRecv(std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - clock::now()));
  }

  // Deadline overload.
  template <typename clock>
  bool waitSend(std::chrono::time_point<clock> deadline) {
    return waitSend(std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - clock::now()));
  }

  // If the byte count argument is not specified, it will default the
  // number of bytes to be equal to the number of bytes remaining in
  // the buffer w.r.t. the offset.
  static constexpr auto kUnspecifiedByteCount =
      std::numeric_limits<size_t>::max();

  virtual void send(
      int dstRank,
      uint64_t slot,
      size_t offset = 0,
      size_t nbytes = kUnspecifiedByteCount) = 0;

  virtual void recv(
      int srcRank,
      uint64_t slot,
      size_t offset = 0,
      size_t nbytes = kUnspecifiedByteCount) = 0;

  virtual void recv(
      std::vector<int> srcRanks,
      uint64_t slot,
      size_t offset = 0,
      size_t nbytes = kUnspecifiedByteCount) = 0;

  virtual std::unique_ptr<RemoteKey> getRemoteKey() const {
    throw std::runtime_error("getRemoteKey() not implemented");
  }

  // Asymmetrically send data to the given remote key.
  // Call waitSend to wait on the results of this.
  virtual void put(
      const RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) {
    throw std::runtime_error("put(RemoteKey) not implemented");
  }

  // Asymmetrically receive data from the given remote key.
  // Call waitRecv to wait on the results of this.
  virtual void get(
      const RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) {
    throw std::runtime_error("get(RemoteKey) not implemented");
  }
};

} // namespace transport
} // namespace gloo
