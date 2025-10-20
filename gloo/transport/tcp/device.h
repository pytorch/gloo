/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <gloo/transport/device.h>
#include <gloo/transport/tcp/attr.h>
#include <gloo/transport/tcp/error.h>
#include <gloo/transport/tcp/listener.h>
#include <gloo/transport/tcp/loop.h>
#include <gloo/transport/tcp/socket.h>

namespace gloo {
namespace transport {
namespace tcp {

struct attr CreateDeviceAttr(const struct attr& src);

std::shared_ptr<::gloo::transport::Device> CreateDevice(const struct attr&);
std::shared_ptr<::gloo::transport::Device> CreateLazyDevice(const struct attr&);

// Forward declarations
class Pair;
class Buffer;

class Device : public ::gloo::transport::Device,
               public std::enable_shared_from_this<Device> {
 public:
  explicit Device(const struct attr& attr, bool lazyInit);
  virtual ~Device();

  virtual std::string str() const override;

  virtual const std::string& getPCIBusID() const override;

  virtual int getInterfaceSpeed() const override;

  virtual std::shared_ptr<::gloo::transport::Context> createContext(
      int rank,
      int size) override;

  void registerDescriptor(int fd, int events, Handler* h);
  void unregisterDescriptor(int fd, Handler* h);

  // TCP is bidirectional so when we connect two ends of a pair,
  // one side is the connection initiator and the other is the listener.
  bool isInitiator(const Address& local, const Address& remote) const;

  void shutdown();

  bool isLazyInit() {
    return lazyInit_;
  }

 protected:
  const struct attr attr_;
  const bool lazyInit_;

  // Return a new `Address` instance.
  //
  // This is called by the constructor of the `Pair` class. It gives
  // the pair a uniquely identifying address even though the device
  // uses a shared listening socket.
  //
  Address nextAddress();

  // Return a new `Address` instance using the provided sequence number.
  //
  // This is called by the constructor of the `Pair` class. It gives
  // the pair a uniquely identifying address even though the device
  // uses a shared listening socket. Caller must provide a unique sequence
  // number
  //
  Address nextAddress(int);

  // Connect a pair to a remote.
  //
  // This is performed by the device instance because we use a single
  // listening socket for all inbound pair connections.
  //
  // Matching these connections with pairs is done with a handshake.
  // The remote side of the connection writes a sequence number (see
  // `Address::sequence_t`) to the stream that identifies the pair
  // it wants to connect to. On the local side, this sequence number
  // is read and used as key in a map with callbacks. If the callback
  // is found, it is called. If the callback is not found, the
  // connection is cached in a map, using the sequence number.
  //
  using connect_callback_t =
      std::function<void(std::shared_ptr<Socket> socket, const Error& error)>;

  void connect(
      const Address& local,
      const Address& remote,
      const int rank,
      const int size,
      std::chrono::milliseconds timeout,
      connect_callback_t fn);

  void connectAsListener(
      const Address& local,
      std::chrono::milliseconds timeout,
      connect_callback_t fn);

  void connectAsInitiator(
      const Address& remote,
      const int rank,
      const int size,
      std::chrono::milliseconds timeout,
      connect_callback_t fn);

  friend class Pair;
  friend class Buffer;

 private:
  std::shared_ptr<Loop> loop_;
  std::shared_ptr<Listener> listener_;

  std::string interfaceName_;
  int interfaceSpeedMbps_;
  std::string pciBusID_;
};

} // namespace tcp
} // namespace transport
} // namespace gloo
