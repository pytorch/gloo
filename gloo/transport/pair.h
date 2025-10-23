/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include "gloo/common/enforce.h"
#include "gloo/transport/address.h"
#include "gloo/transport/buffer.h"
#include "gloo/transport/unbound_buffer.h"

namespace gloo {
namespace transport {

class Pair {
 public:
  virtual ~Pair() = 0;

  virtual const Address& address() const = 0;

  virtual void connect(const std::vector<char>& bytes) = 0;

  virtual void close() = 0;

  virtual void setSync(bool enable, bool busyPoll) = 0;

  virtual std::unique_ptr<Buffer> createSendBuffer(
      int slot,
      void* ptr,
      size_t size) = 0;

  virtual std::unique_ptr<Buffer> createRecvBuffer(
      int slot,
      void* ptr,
      size_t size) = 0;

  virtual bool isConnected() = 0;

  // Send from the specified buffer to remote side of pair.
  virtual void send(
      UnboundBuffer* buf,
      uint64_t tag,
      size_t offset = 0,
      size_t nbytes = 0) = 0;

  // Receive into the specified buffer from the remote side of pair.
  virtual void recv(
      UnboundBuffer* buf,
      uint64_t tag,
      size_t offset = 0,
      size_t nbytes = 0) = 0;

  virtual void put(
      transport::UnboundBuffer* tbuf,
      const transport::RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) {
    throw std::runtime_error("put() not implemented");
  }

  virtual void get(
      transport::UnboundBuffer* tbuf,
      const transport::RemoteKey& key,
      uint64_t slot,
      size_t offset,
      size_t roffset,
      size_t nbytes) {
    throw std::runtime_error("get() not implemented");
  }

  // Sets the local rank of the process to be localRank
  // (See below for description of local rank)
  void setLocalRank(int localRank) {
    // Local rank should be a non-negative number
    GLOO_ENFORCE(localRank >= 0, "LocalRank must be non-negative");

    localRank_ = localRank;
  }

  // Returns the local rank of the process
  // (See below for description of local rank)
  int getLocalRank() const {
    return localRank_;
  }

 protected:
  // Rank of the process on the local machine
  // e.g. Suppose we have 2 machines with 8 GPUs per machine.
  //      This means we have a total of 16 processes with
  //      global ranks 0 to 15. The local ranks would then
  //      be 0 to 7 on each machine.
  int localRank_;
};

} // namespace transport
} // namespace gloo
