/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>
#include <unordered_map>

#include <gloo/transport/tcp/address.h>
#include <gloo/transport/tcp/attr.h>
#include <gloo/transport/tcp/error.h>
#include <gloo/transport/tcp/loop.h>
#include <gloo/transport/tcp/socket.h>

namespace gloo {
namespace transport {
namespace tcp {

// Listener deals with incoming connections. Incoming connections
// write a few bytes containing a sequence number. This sequence
// number is read off the socket and matched to a local sequence
// number. If there is a match, the socket is passed to the
// corresponding pair. If it can't be matched, it is stashed until a
// pair with the sequence number calls `waitForConnection`.
class Listener final : public Handler {
 public:
  using connect_callback_t =
      std::function<void(std::shared_ptr<Socket> socket, const Error& error)>;

  static constexpr int kBacklog = -1; // allow somaxconn

  Listener(std::shared_ptr<Loop> loop, const attr& attr);

  ~Listener() override;

  void handleEvents(Loop& loop, int events) override;

  Address nextAddress();

  Address nextAddress(int);

  // Wait for connection with sequence number `seq`. The callback is
  // always called from a different thread (the event loop thread),
  // even if the connection is already available.
  void waitForConnection(sequence_number_t seq, connect_callback_t fn);

  void shutdown();

 private:
  std::mutex mutex_;
  std::shared_ptr<Loop> loop_;
  std::shared_ptr<Socket> listener_;
  std::shared_ptr<std::atomic<bool>> closed_{
      std::make_shared<std::atomic<bool>>(false)};

  // Address of this listener and the sequence number for the next
  // connection. Sequence numbers are written by a peer right after
  // establishing a new connection and used locally to match a new
  // connection to a pair instance.
  Address addr_;
  sequence_number_t seq_{0};

  // Called when we've read a sequence number from a new socket.
  void haveConnection(std::shared_ptr<Socket> socket, sequence_number_t seq);

  // Callbacks by sequence number (while waiting for a connection).
  std::unordered_map<sequence_number_t, connect_callback_t> seqToCallback_;

  // Sockets by sequence number (while waiting for a pair to call).
  std::unordered_map<sequence_number_t, std::shared_ptr<Socket>> seqToSocket_;

  // Option to use rank as sequence number and avoid pair identifiers
  // to the store during rendezvous. Experimental, disabled by default.
  // Can be enabled by setting the environment variable
  // GLOO_ENABLE_RANK_AS_SEQUENCE_NUMBER=1.
  bool useRankAsSeqNumber_{false};
};

} // namespace tcp
} // namespace transport
} // namespace gloo
