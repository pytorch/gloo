/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>

#include "gloo/transport/tcp/error.h"
#include "gloo/transport/tcp/loop.h"
#include "gloo/transport/tcp/socket.h"

namespace gloo::transport::tcp {

// ReadValueOperation asynchronously reads a value of type T from the
// socket specified at construction. Upon completion or error, the
// callback is called. Its lifetime is coupled with completion of the
// operation, so the called doesn't need to hold on to the instance.
template <typename T>
class ReadValueOperation final
    : public Handler,
      public std::enable_shared_from_this<ReadValueOperation<T>> {
 public:
  using callback_t =
      std::function<void(std::shared_ptr<Socket>, const Error& error, T&& t)>;

  ReadValueOperation(std::shared_ptr<Socket> socket, callback_t fn)
      : socket_(std::move(socket)), fn_(std::move(fn)) {}

  void run(Loop& loop) {
    loop.registerDescriptor(
        socket_->fd(), EPOLLIN | EPOLLONESHOT, this->shared_from_this());
  }

  void handleEvents(Loop&, int /*events*/) override {
    // Read T.
    auto rv = socket_->read(&t_, sizeof(t_));
    if (rv == -1) {
      fn_(socket_,
          SystemError("read", errno, socket_->safePeerName()),
          std::move(t_));
      return;
    }

    // Check for short read (assume we can read in a single call).
    if (rv < sizeof(t_)) {
      fn_(socket_,
          ShortReadError(rv, sizeof(t_), socket_->safePeerName()),
          std::move(t_));
      return;
    }

    fn_(socket_, Error::kSuccess, std::move(t_));
  }

 private:
  std::shared_ptr<Socket> socket_;
  callback_t fn_;

  T t_;
};

template <typename T>
void read(
    Loop& loop,
    std::shared_ptr<Socket> socket,
    typename ReadValueOperation<T>::callback_t fn) {
  auto x =
      std::make_shared<ReadValueOperation<T>>(std::move(socket), std::move(fn));
  x->run(loop);
}

// WriteValueOperation asynchronously writes a value of type T to the
// socket specified at construction. Upon completion or error, the
// callback is called. Its lifetime is coupled with completion of the
// operation, so the called doesn't need to hold on to the instance.
template <typename T>
class WriteValueOperation final
    : public Handler,
      public std::enable_shared_from_this<WriteValueOperation<T>> {
 public:
  using callback_t =
      std::function<void(std::shared_ptr<Socket>, const Error& error)>;

  WriteValueOperation(std::shared_ptr<Socket> socket, T t, callback_t fn)
      : socket_(std::move(socket)), fn_(std::move(fn)), t_(std::move(t)) {}

  void run(Loop& loop) {
    loop.registerDescriptor(
        socket_->fd(), EPOLLOUT | EPOLLONESHOT, this->shared_from_this());
  }

  void handleEvents(Loop&, int /*events*/) override {
    // Write T.
    auto rv = socket_->write(&t_, sizeof(t_));
    if (rv == -1) {
      fn_(socket_, SystemError("write", errno, socket_->safePeerName()));
      return;
    }

    // Check for short write (assume we can write in a single call).
    if (rv < sizeof(t_)) {
      fn_(socket_, ShortWriteError(rv, sizeof(t_), socket_->safePeerName()));
      return;
    }

    fn_(socket_, Error::kSuccess);
  }

 private:
  std::shared_ptr<Socket> socket_;
  callback_t fn_;

  T t_;
};

template <typename T>
void write(
    Loop& loop,
    std::shared_ptr<Socket> socket,
    T t,
    typename WriteValueOperation<T>::callback_t fn) {
  auto x = std::make_shared<WriteValueOperation<T>>(
      std::move(socket), std::move(t), std::move(fn));
  x->run(loop);
}

class ConnectOperation final
    : public Handler,
      public std::enable_shared_from_this<ConnectOperation> {
 public:
  using callback_t = std::function<
      void(Loop& loop, std::shared_ptr<Socket>, const Error& error)>;
  ConnectOperation(
      const Address& remote,
      const int rank,
      const int size,
      std::chrono::milliseconds timeout,
      callback_t fn)
      : remote_(remote),
        rank_(rank),
        size_(size),
        deadline_(std::chrono::steady_clock::now() + timeout),
        fn_(std::move(fn)) {}

  void run(Loop& loop) {
    const auto& sockaddr = remote_.getSockaddr();

    // Create new socket to connect to peer.
    socket_ = Socket::createForFamily(sockaddr.ss_family);
    socket_->reuseAddr(true);
    socket_->noDelay(true);
    socket_->connect(sockaddr);

    // Register for EPOLLOUT, because we want to be notified when
    // the connect completes. EPOLLERR is also necessary because
    // connect() can fail.
    loop.registerDescriptor(
        socket_->fd(),
        EPOLLOUT | EPOLLERR | EPOLLONESHOT,
        this->shared_from_this());
  }

  void handleEvents(Loop& loop, int /*events*/) override;

 private:
  const Address remote_;
  const int rank_;
  const int size_;
  const std::chrono::time_point<std::chrono::steady_clock> deadline_;
  const int maxRetries_{3};

  int retry_{0};

  std::shared_ptr<Socket> socket_;
  callback_t fn_;
};

void connectLoop(
    Loop& loop,
    const Address& remote,
    const int rank,
    const int size,
    std::chrono::milliseconds timeout,
    typename ConnectOperation::callback_t fn);

} // namespace gloo::transport::tcp
