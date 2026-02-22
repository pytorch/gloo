/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "gloo/common/memory.h"
#include "gloo/common/store.h"
#include "gloo/transport/context.h"
#include "gloo/transport/tcp/peel/peel_handshake.h" // PEEL

namespace gloo {
namespace transport {
namespace tcp {

// Forward declaration
class Context;
class Device;
class Pair;
class UnboundBuffer;

class Context : public ::gloo::transport::Context,
                public std::enable_shared_from_this<Context> {
 public:
  Context(std::shared_ptr<Device> device, int rank, int size);

  virtual ~Context();



  // ---------------------------------------------------------------------------
  // Peel Handshake Support
  // ---------------------------------------------------------------------------

  // Enable Peel handshake capability with given configuration
  // Call this before performPeelHandshake()
  void enablePeel(const peel::PeelConfig& config);

  // Perform the Peel handshake
  // - isSender: true if this rank is the sender (typically rank 0)
  // - expectedReceivers: number of receivers to wait for (only used if isSender)
  // Returns true on success
  bool performPeelHandshake(bool isSender, int expectedReceivers = 0);

  // Check if Peel is enabled
  bool isPeelEnabled() const { return peelHandshake_ != nullptr || peelCohort_ != nullptr; }

  // Check if Peel handshake completed successfully
  bool isPeelReady() const { return peelCohort_ != nullptr; }

  // Get the Peel cohort after successful handshake
  // Returns nullptr if handshake not done or failed
  const peel::PeelCohort* peelCohort() const { return peelCohort_.get(); }

  // Get mutable Peel cohort (for taking ownership of socket, etc.)
  peel::PeelCohort* peelCohortMutable() { return peelCohort_.get(); }






  virtual void createAndConnectAllPairs(std::shared_ptr<IStore> store) override;

  std::unique_ptr<transport::Pair>& createPair(int rank) override;
  std::unique_ptr<transport::Pair>& createPair(
      int rank,
      bool useRankAsSeqNumber);

  virtual std::unique_ptr<transport::Pair>& getPair(int rank) override;

  std::unique_ptr<transport::UnboundBuffer> createUnboundBuffer(
      void* ptr,
      size_t size) override;

 protected:

  // ---------------------------------------------------------------------------
  // Peel Handshake Members
  // ---------------------------------------------------------------------------

  // Peel handshake object (non-null while handshake in progress)
  std::unique_ptr<peel::PeelHandshake> peelHandshake_;

  // Peel cohort result (non-null after successful handshake)
  std::unique_ptr<peel::PeelCohort> peelCohort_;



  

  std::shared_ptr<Device> device_;
  std::shared_ptr<IStore> store_{nullptr};

  // Protects the connection states to avoid race conditions.
  std::mutex m_;
  // Whether or not connection has been started for this peer.
  std::vector<bool> connecting_;

  using pendingRecvTuple = std::tuple<
      WeakNonOwningPtr<UnboundBuffer>,
      size_t,
      size_t,
      std::unordered_set<int>>;

  // Buffers with pending receive operation by slot.
  std::unordered_map<uint64_t, std::deque<pendingRecvTuple>> pendingRecv_;

  // This function registers the specified unbound buffer for a receive
  // operation from any of the specified ranks.
  void recvFromAny(
      UnboundBuffer* buf,
      uint64_t slot,
      size_t offset,
      size_t nbytes,
      std::vector<int> srcRanks);

  int recvFromAnyFindRank(
      UnboundBuffer* buf,
      uint64_t slot,
      size_t offset,
      size_t nbytes,
      const std::vector<int>& srcRanks);

  // Allowed to be called only by ContextMutator::findRecvFromAny,
  // where the context lock is already held.
  bool findRecvFromAny(
      uint64_t slot,
      int rank,
      WeakNonOwningPtr<tcp::UnboundBuffer>* buf,
      size_t* offset,
      size_t* nbytes);

  // Set exception on every pair in this context. This is called when
  // waiting for a send or recv operation on an unbound buffer times
  // out. All pairs should be signaled and closed in that event.
  void signalException(const std::string& msg);

  // Returns a sorted list of connected peer ranks, excluding self.
  // Normally all peer ranks should be connected at the end of
  // createAndConnectAllPairs
  // peer rank concept is introduced at the transport/tcp/pair level, so
  // this method is defined at the same level instead of the parent contexts
  std::vector<int> getConnectedPeerRanks() const;

  // Returns a sorted list of unconnected and peer ranks, excluding self.
  // Normally empty at the end of createAndConnectAllPairs
  std::vector<int> getUnConnectedPeerRanks() const;

  // a helper function to print out rank to rank connectivity information
  void printConnectivityInfo() const;

  friend class ContextMutator;

  friend class UnboundBuffer;

  friend class Pair;
};

struct Rank {
  std::string hostname;
  std::vector<char> addressBytes;
  std::vector<ssize_t> pairIdentifiers;

  explicit Rank(
      const std::string& hostname,
      const std::vector<char>& addrBytes,
      const std::vector<ssize_t>& pairIdentifiers)
      : hostname(hostname),
        addressBytes(addrBytes),
        pairIdentifiers(pairIdentifiers) {}
  explicit Rank(const std::vector<char>& bytes);

  std::vector<char> bytes() const;
};

} // namespace tcp
} // namespace transport
} // namespace gloo
