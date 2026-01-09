/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <sys/types.h>

#include <gloo/transport/device.h>
#include <gloo/transport/uv/address.h>

#ifndef _WIN32
#include <sys/socket.h>
#endif

namespace gloo {
namespace transport {
namespace uv {

// Describes how to resolve address to bind device to. It can use
// either a hostname, a sockaddr struct, or the string name of a
// network interface. Whatever is used will be resolved to a
// sockaddr_storage struct to finally bind a socket to.
struct attr {
  attr() = default;
  /* implicit */ attr(const char* ptr) : hostname(ptr) {}

  std::string hostname;

  std::string iface;

  // The address family defaults to AF_UNSPEC such that getaddrinfo(3)
  // will try to find either IPv4 or IPv6 addresses.
  int ai_family = AF_UNSPEC;
  int ai_socktype;
  int ai_protocol;
  struct sockaddr_storage ai_addr;
  int ai_addrlen;
};

// Forward declarations.
class Pair;

// The libuv wrapper classes must remain incomplete types in this
// header. Otherwise, downstream users would be forced to depend on
// the wrapper's headers as well as libuv themselves.
namespace libuv {
class CloseEvent;
class EndEvent;
class ErrorEvent;
class ReadEvent;
class WriteEvent;
class Loop;
class Async;
class TCP;
} // namespace libuv

// Create and return a device instance per the specified attributes.
std::shared_ptr<::gloo::transport::Device> CreateDevice(struct attr);

// Device instance represents a single I/O thread, libuv event loop,
// and socket that this process is listening on for incoming
// connections. A device may be reused across multiple contexts.
class Device : public ::gloo::transport::Device,
               public std::enable_shared_from_this<Device> {
 public:
  explicit Device(struct attr attr);

  ~Device() override;

  std::string str() const override;

  const std::string& getPCIBusID() const override;

  std::shared_ptr<::gloo::transport::Context> createContext(int rank, int size)
      override;

 protected:
  using ConnectCallback = std::function<
      void(std::shared_ptr<libuv::TCP>, const libuv::ErrorEvent&)>;

  // Return a new `Address` instance.
  //
  // This is called by the constructor of the `Pair` class. It gives
  // the pair a uniquely identifying address even though the device
  // uses a single shared listening socket.
  //
  Address nextAddress();

  friend class Pair;

  // Connect a pair to a remote.
  //
  // This is performed by the device instance because we use a single
  // listening socket for all inbound pair connections.
  //
  // Matching these connections with pairs is done with a handshake.
  // The remote side of the connection writes a sequence number (see
  // `Address::sequence_type`) to the stream that identifies the pair
  // it wants to connect to. On the local side, this sequence number
  // is read and used as key in a map with callbacks. If the callback
  // is found, it is called. If the callback is not found, the
  // connection is cached in a map, using the sequence number.
  //
  void connect(
      const Address& local,
      const Address& remote,
      std::chrono::milliseconds timeout,
      ConnectCallback fn);

  void connectAsListener(
      const Address& local,
      std::chrono::milliseconds timeout,
      ConnectCallback fn);

  void connectAsListenerCallback(
      std::shared_ptr<libuv::TCP> handle,
      const libuv::ReadEvent& event);

  void connectAsInitiator(
      const Address& remote,
      std::chrono::milliseconds timeout,
      ConnectCallback fn);

 private:
  std::mutex mutex_;

  // Copy of `struct attr` this instance was constructed with.
  const struct attr attr_;

  // PCI bus ID for this device's interface.
  // Not used at this time but necessary to have as a member field
  // because the base class defines a getter function that returns a
  // const reference.
  const std::string pciBusID_;

  // A device instance has its own event loop.
  std::shared_ptr<libuv::Loop> loop_;

  // This is used so functions can run on the loop thread.
  std::shared_ptr<libuv::Async> async_;

  // The endpoint that peers connect to.
  std::shared_ptr<libuv::TCP> listener_;

  // The address of the listening socket.
  Address addr_;

  // A sequence number used to give every pair a unique address,
  Address::sequence_type addressSequence_ = 0;

  // Pending connections.
  //
  // Populated by incoming connections for which the local
  // pair hasn't called the `connect` function yet.
  //
  std::unordered_map<Address::sequence_type, std::shared_ptr<libuv::TCP>>
      pendingConnections_;

  // Pending connect callbacks.
  //
  // Populated by connect callbacks for local pairs for which the
  // remote side hasn't connected yet.
  //
  std::unordered_map<Address::sequence_type, ConnectCallback>
      pendingConnectCallbacks_;

  // Event loop thread.
  std::unique_ptr<std::thread> thread_;

  // Temporary storage of functions that are scheduled to run on the
  // next event loop tick. Also see `defer()`.
  std::vector<std::function<void()>> deferred_;

  // Defer the specified function to run on the event loop thread.
  void defer(std::function<void()> fn);

  // Defer the specified function to run on the event loop thread.
  void defer_CALL_THIS_WHILE_HOLDING_DEVICE_LOCK(std::function<void()> fn);

  // Called by event loop when deferred functions can be executed.
  void asyncCallback();

  // Called by event loop when a new connection to this device's
  // listening socket was made.
  void listenCallback();
};

} // namespace uv
} // namespace transport
} // namespace gloo
