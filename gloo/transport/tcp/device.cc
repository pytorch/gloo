/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/tcp/device.h"

#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <array>
#include <iostream>

#include "gloo/common/enforce.h"
#include "gloo/common/error.h"
#include "gloo/common/linux.h"
#include "gloo/common/utils.h"
#include "gloo/transport/tcp/context.h"
#include "gloo/transport/tcp/helpers.h"
#include "gloo/transport/tcp/pair.h"

namespace gloo {
namespace transport {
namespace tcp {

static void lookupAddrForIface(struct attr& attr) {
  struct ifaddrs* ifap;
  auto rv = getifaddrs(&ifap);
  GLOO_ENFORCE_NE(rv, -1, strerror(errno));
  struct ifaddrs* ifa;
  for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
    // Skip entry if ifa_addr is NULL (see getifaddrs(3))
    if (ifa->ifa_addr == nullptr) {
      continue;
    }
    // Skip entry if the name doesn't match
    if (strcmp(attr.iface.c_str(), ifa->ifa_name) != 0) {
      continue;
    }
    // Match on address family
    switch (attr.ai_family) {
      case AF_INET:
        if (ifa->ifa_addr->sa_family != AF_INET) {
          continue;
        }
        attr.ai_addrlen = sizeof(struct sockaddr_in);
        memcpy(&attr.ai_addr, ifa->ifa_addr, attr.ai_addrlen);
        break;
      case AF_INET6:
        if (ifa->ifa_addr->sa_family != AF_INET6) {
          continue;
        }
        attr.ai_addrlen = sizeof(struct sockaddr_in6);
        memcpy(&attr.ai_addr, ifa->ifa_addr, attr.ai_addrlen);
        break;
      case AF_UNSPEC:
        switch (ifa->ifa_addr->sa_family) {
          case AF_INET:
            attr.ai_family = AF_INET;
            attr.ai_addrlen = sizeof(struct sockaddr_in);
            break;
          case AF_INET6:
            attr.ai_family = AF_INET6;
            attr.ai_addrlen = sizeof(struct sockaddr_in6);
            break;
          default:
            continue;
        }
        memcpy(&attr.ai_addr, ifa->ifa_addr, attr.ai_addrlen);
        break;
      default:
        GLOO_ENFORCE(false, "Unknown ai_family: ", attr.ai_family);
        break;
    }

    attr.ai_socktype = SOCK_STREAM;
    attr.ai_protocol = 0;
    break;
  }
  GLOO_ENFORCE(ifa != nullptr, "Unable to find address for: ", attr.iface);
  freeifaddrs(ifap);
  return;
}

static void lookupAddrForHostname(struct attr& attr) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = attr.ai_family;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result;
  int bind_rv = 0;
  int bind_errno = 0;
  std::string bind_addr;
  auto rv = getaddrinfo(attr.hostname.data(), nullptr, &hints, &result);
  GLOO_ENFORCE_EQ(rv, 0);
  struct addrinfo* rp;
  for (rp = result; rp != nullptr; rp = rp->ai_next) {
    auto fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    bind_rv = bind(fd, rp->ai_addr, rp->ai_addrlen);
    if (bind_rv == -1) {
      bind_errno = errno;
      bind_addr = Address(rp->ai_addr, rp->ai_addrlen).str();
      close(fd);
      continue;
    }

    attr.ai_family = rp->ai_family;
    attr.ai_socktype = rp->ai_socktype;
    attr.ai_protocol = rp->ai_protocol;
    memcpy(&attr.ai_addr, rp->ai_addr, rp->ai_addrlen);
    attr.ai_addrlen = rp->ai_addrlen;
    close(fd);
    break;
  }

  // If the final call to bind(2) failed, raise error saying so.
  GLOO_ENFORCE(
      bind_rv == 0,
      "Unable to find address for ",
      attr.hostname,
      "; bind(2) for ",
      bind_addr,
      " failed with: ",
      strerror(bind_errno));

  // Verify that we were able to find an address in the first place.
  GLOO_ENFORCE(rp != nullptr, "Unable to find address for: ", attr.hostname);
  freeaddrinfo(result);
  return;
}

struct attr CreateDeviceAttr(const struct attr& src) {
  struct attr attr = src;
  if (attr.iface.size() > 0) {
    // Initialize attributes using network interface name
    lookupAddrForIface(attr);
  } else {
    // Initialize attributes using hostname/IP address
    // If not already specified, use this machine's hostname
    if (attr.hostname.size() == 0) {
      std::array<char, HOST_NAME_MAX> hostname;
      auto rv = gethostname(hostname.data(), hostname.size());
      GLOO_ENFORCE_EQ(rv, 0);
      attr.hostname = hostname.data();
    }
    lookupAddrForHostname(attr);
  }
  return attr;
}

std::shared_ptr<transport::Device> CreateDevice(const struct attr& src) {
  auto device =
      std::make_shared<Device>(CreateDeviceAttr(src), /*lazyInit=*/false);
  return std::shared_ptr<transport::Device>(device);
}

std::shared_ptr<transport::Device> CreateLazyDevice(const struct attr& src) {
  auto device =
      std::make_shared<Device>(CreateDeviceAttr(src), /*lazyInit=*/true);
  return std::shared_ptr<transport::Device>(device);
}

bool isLocalhostAddr(const struct sockaddr* addr) {
  if (addr->sa_family == AF_INET) {
    // Check if the address is in the range '127.x.x.x'
    auto in = (struct sockaddr_in*)addr;
    auto mask = htonl(IN_CLASSA_NET);
    auto subnet = htonl(INADDR_LOOPBACK) & mask;
    return (in->sin_addr.s_addr & mask) == subnet;
  }
  return false;
}

const std::string sockaddrToInterfaceName(const struct attr& attr) {
  struct ifaddrs* ifap;
  std::string iface;
  auto rv = getifaddrs(&ifap);
  GLOO_ENFORCE_NE(rv, -1, strerror(errno));
  auto addrIsLocalhost = isLocalhostAddr((struct sockaddr*)&attr.ai_addr);
  struct ifaddrs* ifa;
  for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
    // Skip entry if ifa_addr is NULL (see getifaddrs(3))
    if (ifa->ifa_addr == nullptr) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET) {
      auto sz = sizeof(struct sockaddr_in);
      // Check if this interface address matches the provided address, or if
      // this is the localhost interface and the provided address is in the
      // localhost subnet.
      if ((memcmp(&attr.ai_addr, ifa->ifa_addr, sz) == 0) ||
          (addrIsLocalhost && isLocalhostAddr(ifa->ifa_addr))) {
        iface = ifa->ifa_name;
        break;
      }
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
      auto sz = sizeof(struct sockaddr_in6);
      if (memcmp(&attr.ai_addr, ifa->ifa_addr, sz) == 0) {
        iface = ifa->ifa_name;
        break;
      }
    }
  }
  GLOO_ENFORCE(
      ifa != nullptr,
      "Unable to find interface for: ",
      Address(attr.ai_addr).str());
  freeifaddrs(ifap);
  return iface;
}

Device::Device(const struct attr& attr, bool lazyInit)
    : attr_(attr),
      lazyInit_(lazyInit),
      loop_(std::make_shared<Loop>()),
      listener_(std::make_shared<Listener>(loop_, attr)),
      interfaceName_(sockaddrToInterfaceName(attr_)),
      interfaceSpeedMbps_(getInterfaceSpeedByName(interfaceName_)),
      pciBusID_(interfaceToBusID(interfaceName_)) {}

void Device::shutdown() {
  loop_->shutdown();
  listener_->shutdown();
}

Device::~Device() {
  shutdown();
}

std::string Device::str() const {
  std::stringstream ss;
  ss << "tcp";
  ss << ", pci=" << pciBusID_;
  ss << ", iface=" << interfaceName_;
  ss << ", speed=" << interfaceSpeedMbps_;
  ss << ", addr=" << Address(attr_.ai_addr).str();
  return ss.str();
}

const std::string& Device::getPCIBusID() const {
  return pciBusID_;
}

int Device::getInterfaceSpeed() const {
  return interfaceSpeedMbps_;
}

std::shared_ptr<transport::Context> Device::createContext(int rank, int size) {
  return std::shared_ptr<transport::Context>(
      new tcp::Context(shared_from_this(), rank, size));
}

void Device::registerDescriptor(int fd, int events, Handler* h) {
  loop_->registerDescriptor(fd, events, h);
}

void Device::unregisterDescriptor(int fd, Handler* h) {
  loop_->unregisterDescriptor(fd, h);
}

Address Device::nextAddress() {
  return listener_->nextAddress();
}

Address Device::nextAddress(int seq) {
  return listener_->nextAddress(seq);
}

bool Device::isInitiator(const Address& local, const Address& remote) const {
  int rv = 0;
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
    rv = memcmp(&sa->sin_addr, &sb->sin_addr, sizeof(struct in_addr));
    if (rv == 0) {
      rv = sa->sin_port - sb->sin_port;
    }
  } else if (family == AF_INET6) {
    const struct sockaddr_in6* sa = (struct sockaddr_in6*)&ss1;
    const struct sockaddr_in6* sb = (struct sockaddr_in6*)&ss2;
    rv = memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(struct in6_addr));
    if (rv == 0) {
      rv = sa->sin6_port - sb->sin6_port;
    }
  } else {
    GLOO_ENFORCE(false, "Unknown address family: ", family);
  }

  // If both sides of the pair use the same address and port, they are
  // sharing the same device instance. This happens in tests. Compare
  // sequence number to allow pairs to connect.
  if (rv == 0) {
    rv = local.getSeq() - remote.getSeq();
  }
  GLOO_ENFORCE_NE(rv, 0, "Cannot connect to self");
  return rv > 0;
}

void Device::connect(
    const Address& local,
    const Address& remote,
    const int rank,
    const int size,
    std::chrono::milliseconds timeout,
    connect_callback_t fn) {
  auto initiator = isInitiator(local, remote);

  if (initiator) {
    connectAsInitiator(remote, rank, size, timeout, std::move(fn));
    return;
  }
  connectAsListener(local, timeout, std::move(fn));
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
    std::chrono::milliseconds /* unused */,
    connect_callback_t fn) {
  // TODO(pietern): Use timeout.
  listener_->waitForConnection(local.getSeq(), std::move(fn));
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
    const int rank,
    const int size,
    std::chrono::milliseconds timeout,
    connect_callback_t fn) {
  auto writeSeq =
      [seq = remote.getSeq()](
          Loop& loop, std::shared_ptr<Socket> socket, connect_callback_t fn) {
        // Write sequence number for peer to new socket.
        write<sequence_number_t>(loop, std::move(socket), seq, std::move(fn));
      };

  if (disableConnectionRetries()) {
    const auto& sockaddr = remote.getSockaddr();

    // Create new socket to connect to peer.
    auto socket = Socket::createForFamily(sockaddr.ss_family);
    socket->reuseAddr(true);
    socket->noDelay(true);
    socket->connect(sockaddr);

    writeSeq(*loop_, std::move(socket), std::move(fn));
  } else {
    connectLoop(
        *loop_,
        remote,
        rank,
        size,
        timeout,
        [fn = std::move(fn), writeSeq = std::move(writeSeq)](
            Loop& loop, std::shared_ptr<Socket> socket, const Error& error) {
          if (error) {
            fn(socket, error);
            return;
          }

          writeSeq(loop, std::move(socket), fn);
        });
  }
}

} // namespace tcp
} // namespace transport
} // namespace gloo
