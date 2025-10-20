/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/context.h"
#include "gloo/common/utils.h"

namespace gloo {
namespace transport {

Context::Context(int rank, int size) : rank(rank), size(size) {
  pairs_.resize(size);
}

// Have to provide implementation for pure virtual destructor.
Context::~Context() {}

std::unique_ptr<transport::Pair>& Context::getPair(int rank_2) {
  return pairs_.at(rank_2);
}

void Context::createAndConnectAllPairs(std::shared_ptr<IStore> store) {
  // this is the default un-optimized version of the rendezvous protocol
  // where each rank would write N pairs to the store
  // and then for each remote peer load the N addresses
  // and only pick the 1 useful
  // A more efficient version (for transport supporting multiplexing like TCP)
  // can be seen in gloo/transport/tcp/context.cc

  std::vector<char> allBytes;
  int localRank = 0;

  auto localHostName = getHostname();
  // Add global rank <> hostname pair to the Store. This store is then passed
  // to Gloo when connectFullMesh is called, where Gloo uses the global rank <>
  // hostname mapping to compute local ranks.
  std::string localKey("rank_" + std::to_string(rank));
  const std::vector<char> value(localHostName.begin(), localHostName.end());
  store->set(localKey, value);

  for (int i = 0; i < size; i++) {
    if (i == rank) {
      break;
    }

    std::string key("rank_" + std::to_string(i));
    auto val = store->get(key);
    auto hostName = std::string((const char*)val.data(), val.size());

    if (hostName == localHostName) {
      localRank++;
    }
  }

  // Create pairs
  for (int i = 0; i < size; i++) {
    if (i == rank) {
      continue;
    }

    auto& pair = createPair(i);
    pair->setLocalRank(localRank);
    auto addrBytes = pair->address().bytes();
    allBytes.insert(allBytes.end(), addrBytes.begin(), addrBytes.end());
  }

  store->set(std::to_string(rank), allBytes);

  // Connect every pair
  for (int i = 0; i < size; i++) {
    if (i == rank) {
      continue;
    }

    // Wait for address of other side of this pair to become available
    std::ostringstream key;
    key << i;
    store->wait({key.str()}, getTimeout());

    // Connect to other side of this pair
    auto allAddrs = store->get(key.str());
    auto addr = extractAddress(allAddrs, i);
    getPair(i)->connect(addr);
  }
}

std::vector<char> Context::extractAddress(
    const std::vector<char>& allAddrs,
    int i) const {
  // Extract address from the list of all addresses
  int adjRank = (rank > i ? rank - 1 : rank);
  // Adjust for the fact that nodes do not store address for themselves
  int addrSize = allAddrs.size() / (size - 1);
  return std::vector<char>(
      allAddrs.begin() + adjRank * addrSize,
      allAddrs.begin() + (adjRank + 1) * addrSize);
}

Context::LazyTally::LazyTally(std::vector<Tally>& vec, slot_t slot)
    : vec_(vec), slot_(slot), initialized_(false) {}

Context::LazyTally::~LazyTally() {
  // Remove empty tally from vector.
  if (initialized_ && it_ != vec_.end() && it_->empty()) {
    vec_.erase(it_);
  }
}

bool Context::LazyTally::exists() {
  initialize_iterator();
  return it_ != vec_.end();
}

Context::Tally& Context::LazyTally::get() {
  initialize_iterator();
  if (it_ == vec_.end()) {
    vec_.emplace_back(slot_);
    it_ = (vec_.end() - 1);
  }
  return *it_;
}

void Context::LazyTally::initialize_iterator() {
  if (initialized_) {
    return;
  }

  it_ =
      std::find_if(vec_.begin(), vec_.end(), [this](const Context::Tally& op) {
        return op.slot == slot_;
      });
  initialized_ = true;
}

Context::Mutator::Mutator(Context& context, slot_t slot, rank_t rank)
    : lock_(context.mutex_),
      context_(context),
      slot_(slot),
      rank_(rank),
      pendingOperations_(context_.pendingOperations_, slot_),
      expectedNotifications_(context_.expectedNotifications_, slot_) {}

void Context::Mutator::pushRemotePendingRecv() {
  pendingOperations_.get().pushRecv(rank_);
}

void Context::Mutator::pushRemotePendingSend() {
  pendingOperations_.get().pushSend(rank_);
}

bool Context::Mutator::shiftRemotePendingRecv() {
  if (!pendingOperations_.exists()) {
    return false;
  }
  return pendingOperations_.get().shiftRecv(rank_);
}

bool Context::Mutator::shiftRemotePendingSend() {
  if (!pendingOperations_.exists()) {
    return false;
  }
  return pendingOperations_.get().shiftSend(rank_);
}

void Context::Mutator::pushExpectedSendNotification() {
  expectedNotifications_.get().pushSend(rank_);
}

bool Context::Mutator::shiftExpectedSendNotification() {
  if (!expectedNotifications_.exists()) {
    return false;
  }
  return expectedNotifications_.get().shiftSend(rank_);
}

std::vector<Context::Tally>::iterator Context::findPendingOperations(
    slot_t slot) {
  return std::find_if(
      pendingOperations_.begin(),
      pendingOperations_.end(),
      [slot](const Tally& op) { return op.slot == slot; });
}

} // namespace transport
} // namespace gloo
