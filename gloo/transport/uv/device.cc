/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/uv/device.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#include <uv.h> // @manual

#include <gloo/common/error.h>
#ifndef _WIN32
#include <gloo/common/linux.h>
#else
#include <gloo/common/win.h> // @manual
#endif
#include <gloo/common/enforce.h>
#include <gloo/transport/uv/common.h>
#include <gloo/transport/uv/context.h>
#include <gloo/transport/uv/libuv.h>

namespace gloo {
namespace transport {
namespace uv {

namespace {

// RAII wrapper for libuv network interface address acquisition.
class InterfaceAddresses {
 public:
  InterfaceAddresses() {
    auto rv = uv_interface_addresses(&info_, &count_);
    UV_CHECK(rv, "uv_interface_addresses");
  }

  ~InterfaceAddresses() {
    uv_free_interface_addresses(info_, count_);
  }

  size_t size() const {
    return count_;
  }

  const uv_interface_address_t& operator[](int i) const {
    return info_[i];
  }

 protected:
  uv_interface_address_t* info_;
  int count_;
};

static bool lookupAddrForIface(struct attr* attr) {
  InterfaceAddresses addresses;

  for (auto i = 0; i < addresses.size(); i++) {
#ifdef _WIN32
    const auto& address = addresses[i].address;
#else
    const auto& interface = addresses[i];
    const auto& address = interface.address;
#endif

    // Skip entry if the name doesn't match.
#ifdef _WIN32
    if (strcmp(attr->iface.c_str(), addresses[i].name) != 0) {
#else
    if (strcmp(attr->iface.c_str(), interface.name) != 0) {
#endif
      continue;
    }

    // Match on address family
    struct sockaddr* sockaddr = (struct sockaddr*)&address.address4;
    switch (sockaddr->sa_family) {
      case AF_INET:
        if (attr->ai_family != AF_INET && attr->ai_family != AF_UNSPEC) {
          continue;
        }
        attr->ai_addrlen = sizeof(address.address4);
        memcpy(&attr->ai_addr, &address.address4, sizeof(address.address4));
        break;
      case AF_INET6:
        if (attr->ai_family != AF_INET6 && attr->ai_family != AF_UNSPEC) {
          continue;
        }
        attr->ai_addrlen = sizeof(address.address6);
        memcpy(&attr->ai_addr, &address.address6, sizeof(address.address6));
        break;
      default:
        continue;
    }

    attr->ai_socktype = SOCK_STREAM;
    attr->ai_protocol = 0;
    return true;
  }

  return false;
}

static void lookupAddrForHostname(struct attr& attr) {
#ifdef _WIN32
  init_winsock();
#endif

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = attr.ai_family;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result;
  auto rv = getaddrinfo(attr.hostname.data(), nullptr, &hints, &result);
  GLOO_ENFORCE_NE(rv, -1);
  struct addrinfo* rp;
  for (rp = result; rp != nullptr; rp = rp->ai_next) {
    auto fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    rv = bind(fd, rp->ai_addr, rp->ai_addrlen);
    if (rv == -1) {
#ifdef _WIN32
      closesocket(fd);
#else
      close(fd);
#endif
      continue;
    }

    attr.ai_family = rp->ai_family;
    attr.ai_socktype = rp->ai_socktype;
    attr.ai_protocol = rp->ai_protocol;
    memcpy(&attr.ai_addr, rp->ai_addr, rp->ai_addrlen);
    attr.ai_addrlen = rp->ai_addrlen;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    break;
  }

  // Check that we found an address we were able to bind to
  GLOO_ENFORCE(rp != nullptr, "Unable to find address for: ", attr.hostname);
  freeaddrinfo(result);
  return;
}

} // namespace

std::shared_ptr<transport::Device> CreateDevice(struct attr attr) {
  if (!attr.iface.empty()) {
    // Initialize attributes using network interface name
    if (!lookupAddrForIface(&attr)) {
      GLOO_ENFORCE(false, "Unable to find address for: ", attr.iface);
    }
  } else {
    // Initialize attributes using hostname/IP address
    // If not already specified, use this machine's hostname
    if (attr.hostname.size() == 0) {
      std::array<char, UV_MAXHOSTNAMESIZE> hostname;
      size_t size = hostname.size();
      auto rv = uv_os_gethostname(hostname.data(), &size);
      GLOO_ENFORCE_EQ(rv, 0);
      attr.hostname = std::string(hostname.data(), size);
    }
    lookupAddrForHostname(attr);
  }

  return std::make_shared<Device>(attr);
}

Device::Device(const struct attr& attr) : attr_(attr) {
  loop_ = libuv::Loop::create();

  // Use async handle to trigger the event loop to
  // run deferred functions on its thread.
  async_ = loop_->resource<libuv::Async>();
  async_->on<libuv::AsyncEvent>(
      [this](const libuv::AsyncEvent&, const libuv::Async&) {
        this->asyncCallback();
      });

  // Initialize server handle and wait for incoming connections.
  listener_ = loop_->resource<libuv::TCP>();
  listener_->on<libuv::ErrorEvent>(
      [](const libuv::ErrorEvent& event, const libuv::TCP&) {
        // Nothing we can do about errors on the listener socket...
        GLOO_ENFORCE(!event, "Error on listener socket: ", event.what());
      });
  listener_->on<libuv::ListenEvent>(
      [this](const libuv::ListenEvent& event, const libuv::TCP&) {
        listenCallback();
      });

  // Bind socket and start listening for new connections.
  listener_->bind((const struct sockaddr*)&attr_.ai_addr);
  listener_->listen();
  addr_ = Address(listener_->sockname());

  // Run uv_run on private thread.
  thread_.reset(new std::thread([this] { loop_->run(); }));
}

Device::~Device() {
  // Close handles associated with this device.
  defer([this] {
    listener_->close();
    async_->close();
  });

  // Wait for uv_run to return.
  thread_->join();
}

std::string Device::str() const {
  std::stringstream ss;
  ss << "listening on " << addr_.str();
  return ss.str();
}

const std::string& Device::getPCIBusID() const {
  return pciBusID_;
}

std::shared_ptr<transport::Context> Device::createContext(int rank, int size) {
  return std::make_shared<Context>(shared_from_this(), rank, size);
}

Address Device::nextAddress() {
  std::lock_guard<std::mutex> guard(mutex_);
  return addr_.withSeq(addressSequence_++);
}

void Device::connect(
    const Address& local,
    const Address& remote,
    std::chrono::milliseconds timeout,
    ConnectCallback fn) {
  int rv;

  // The remote side of a pair will be called with the same
  // addresses, but in reverse. There should only be a single
  // connection between the two, so we pick one side as the listener
  // and the other side as the connector.
  const auto& ss1 = local.getSockaddr();
  const auto& ss2 = remote.getSockaddr();
  GLOO_ENFORCE_EQ(ss1.ss_family, ss2.ss_family);
  const int family = ss1.ss_family;
  if (family == AF_INET) {
    const struct sockaddr_in* sa = (struct sockaddr_in*)&ss1;
    const struct sockaddr_in* sb = (struct sockaddr_in*)&ss2;
    const auto addrlen = sizeof(struct in_addr);
    rv = memcmp(&sa->sin_addr, &sb->sin_addr, addrlen);
    if (rv == 0) {
      rv = sa->sin_port - sb->sin_port;
    }
  } else if (family == AF_INET6) {
    const struct sockaddr_in6* sa = (struct sockaddr_in6*)&ss1;
    const struct sockaddr_in6* sb = (struct sockaddr_in6*)&ss2;
    const auto addrlen = sizeof(struct in6_addr);
    rv = memcmp(&sa->sin6_addr, &sb->sin6_addr, addrlen);
    if (rv == 0) {
      rv = sa->sin6_port - sb->sin6_port;
    }
  } else {
    FAIL("Unknown address family: ", family);
  }

  // If both sides of the pair use the same address and port, they are
  // sharing the same device instance. This happens in tests. Compare
  // sequence number to allow pairs to connect.
  if (rv == 0) {
    rv = local.getSeq() - remote.getSeq();
  }

  if (rv < 0) {
    connectAsListener(local, timeout, std::move(fn));
  } else if (rv > 0) {
    connectAsInitiator(remote, timeout, std::move(fn));
  } else {
    FAIL("Cannot connect to self");
  }
}

// Connecting as listener is passive.
//
// Register the connect callback to be executed when the other side of
// the pair has connected and identified itself as destined for this
// address. To do so, we register the callback for the sequence number
// associated with the address. If this connection already exists,
// deal with it here.
//
void Device::connectAsListener(
    const Address& local,
    std::chrono::milliseconds timeout,
    ConnectCallback connectCallback) {
  defer([=] {
    decltype(pendingConnections_)::mapped_type pendingConnection;

    // Find pending connection, or stash the connect callback.
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto it = pendingConnections_.find(local.getSeq());
      if (it == pendingConnections_.end()) {
        // TODO(pietern): Handle timeout.
        pendingConnectCallbacks_[local.getSeq()] = std::move(connectCallback);
        return;
      }

      pendingConnection = std::move(it->second);
      pendingConnections_.erase(it);
    }

    // There is an existing pending connection, run connect callback.
    connectCallback(std::move(pendingConnection), libuv::ErrorEvent(0));
  });
}

void Device::connectAsListenerCallback(
    std::shared_ptr<libuv::TCP> pendingConnection,
    const libuv::ReadEvent& event) {
  auto seq = event.as<Address::sequence_type>();
  decltype(pendingConnectCallbacks_)::mapped_type connectCallback;

  // Find connect callback, or stash the pending connection.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pendingConnectCallbacks_.find(seq);
    if (it == pendingConnectCallbacks_.end()) {
      pendingConnections_.emplace(seq, std::move(pendingConnection));
      return;
    }

    connectCallback = std::move(it->second);
    pendingConnectCallbacks_.erase(it);
  }

  // There is an existing connect callback, run it.
  connectCallback(std::move(pendingConnection), libuv::ErrorEvent(0));
}

// Connecting as initiator is active.
//
// The connect callback is fired when the connection to the other side
// of the pair has been made, and the sequence number for this
// connection has been written. If an error occurs at any time, the
// callback is called with an associated error event.
//
void Device::connectAsInitiator(
    const Address& remote,
    std::chrono::milliseconds timeout,
    ConnectCallback fn) {
  defer([=] {
    auto tcp = loop_->resource<libuv::TCP>();
    auto timer = loop_->resource<libuv::Timer>();

    // Enable TCP_NODELAY, which disables Nagle's algorithm.
    tcp->noDelay(true);

    // Call callback with error if an error event fired.
    // In nominal operation, this listener will NOT fire.
    // It must be cleared upon success.
    auto errorListener = tcp->once<libuv::ErrorEvent>(
        [=](const libuv::ErrorEvent& event, libuv::TCP& handle) {
          // If a request against this handle was canceled,
          // it must have been due to the timeout firing.
          // The timeout will call the callback with the
          // UV_ETIMEDOUT status and close the handle.
          if (event.code() != UV_ECANCELED) {
            fn(nullptr, event);
            handle.close();
            timer->close();
          }
        });

    // Write sequence number as soon as the connection is made.
    // In nominal operation, this listener will fire at least once.
    tcp->once<libuv::ConnectEvent>(
        [remote](const libuv::ConnectEvent&, libuv::TCP& handle) {
          handle.write(remote.getSeq());
        });

    // Call callback with success if the sequence number was written.
    // In nominal operation, this listener will fire at least once.
    tcp->once<libuv::WriteEvent>(
        [=](const libuv::WriteEvent&, libuv::TCP& handle) {
          handle.erase(errorListener);
          timer->close();
          fn(handle.shared_from_this(), libuv::ErrorEvent(0));
        });

    timer->once<libuv::TimerEvent>(
        [=](const libuv::TimerEvent&, libuv::Timer& handle) {
          fn(nullptr, libuv::ErrorEvent(ETIMEDOUT));
          tcp->close();
          handle.close();
        });

    timer->start(timeout);
    tcp->connect((const sockaddr&)remote.getSockaddr());
  });
}

void Device::defer(std::function<void()> fn) {
  std::lock_guard<std::mutex> guard(mutex_);
  defer_CALL_THIS_WHILE_HOLDING_DEVICE_LOCK(std::move(fn));
}

void Device::defer_CALL_THIS_WHILE_HOLDING_DEVICE_LOCK(
    std::function<void()> fn) {
  deferred_.push_back(std::move(fn));
  async_->send();
}

void Device::asyncCallback() {
  decltype(deferred_) deferred;

  // Lock device when we move the deferred functions to the stack.
  {
    std::lock_guard<std::mutex> guard(mutex_);
    deferred = std::move(deferred_);
  }

  for (auto& fn : deferred) {
    fn();
  }
}

void Device::listenCallback() {
  auto handle = loop_->resource<libuv::TCP>();
  if (!handle) {
    return;
  }

  // Enable TCP_NODELAY, which disables Nagle's algorithm.
  handle->noDelay(true);

  // This is guaranteed to succeed per uv_listen documentation.
  listener_->accept(*handle);

  // Close client if we see EOF or an error before reading data.
  auto endListener = handle->once<libuv::EndEvent>(
      [](const libuv::EndEvent& event, libuv::TCP& handle) { handle.close(); });
  auto errorListener = handle->once<libuv::ErrorEvent>(
      [](const libuv::ErrorEvent& event, libuv::TCP& handle) {
        handle.close();
      });

  // Wait for remote side to write sequence number.
  handle->once<libuv::ReadEvent>(
      [=](const libuv::ReadEvent& event, libuv::TCP& handle) {
        // Sequence number has been read. Either there is an existing
        // connection callback for this sequence number, or we'll hold
        // on to the handle while we wait for the pair to pass a
        // connection callback for this sequence number. Either way,
        // responsibility for this handle is passed to the pair and we
        // must erase these temporary event listeners.
        handle.erase(endListener);
        handle.erase(errorListener);
        this->connectAsListenerCallback(handle.shared_from_this(), event);
      });

  // Read the sequence number so we can pass connection to the right pair.
  auto len = sizeof(Address::sequence_type);
  auto buf = std::unique_ptr<char[]>(new char[len]);
  handle->read(std::move(buf), len);
}

} // namespace uv
} // namespace transport
} // namespace gloo
