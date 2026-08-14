/**
 * Copyright (c) 2020-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/transport/tcp/pair.h"
#include "gloo/transport/tcp/tls/openssl.h"

namespace gloo {
namespace transport {
namespace tcp {
namespace tls {

class Context;
class Device;

class Pair : public ::gloo::transport::tcp::Pair {
 public:
  explicit Pair(
      Context* context,
      Device* device,
      int rank,
      std::chrono::milliseconds timeout);

  ~Pair() override;

  void handleReadWrite(int events) override;

 protected:
  void waitUntilConnected(std::unique_lock<std::mutex>& lock, bool useTimeout)
      override;

  int handshake();

  bool read() override;

  bool write(Op& op) override;

  void waitUntilSSLConnected(
      std::unique_lock<std::mutex>& lock,
      bool useTimeout);

  void verifyConnected(std::unique_lock<std::mutex>& lock) override;

  void changeState(state nextState) noexcept override;

  SSL* ssl_;
  SSL_CTX* ssl_ctx_; // non-owning pointer
  bool is_ssl_connected_;
  bool fatal_error_occurred_;
};

} // namespace tls
} // namespace tcp
} // namespace transport
} // namespace gloo
