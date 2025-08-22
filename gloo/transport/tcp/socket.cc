/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/tcp/socket.h>

#include <fcntl.h>
#include <netinet/tcp.h>
#include <string.h>
#include <unistd.h>

#include <gloo/common/logging.h>

namespace gloo {
namespace transport {
namespace tcp {

std::shared_ptr<Socket> Socket::createForFamily(sa_family_t ai_family) {
  auto rv = socket(ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
  GLOO_ENFORCE_NE(rv, -1, "socket: ", strerror(errno));
  return std::make_shared<Socket>(rv);
}

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void Socket::reuseAddr(bool on) {
  int value = on ? 1 : 0;
  auto rv = ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
  GLOO_ENFORCE_NE(rv, -1, "setsockopt: ", strerror(errno));
}

void Socket::noDelay(bool on) {
  int value = on ? 1 : 0;
  auto rv = ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
  GLOO_ENFORCE_NE(rv, -1, "setsockopt: ", strerror(errno));
}

void Socket::block(bool on) {
  auto rv = fcntl(fd_, F_GETFL);
  GLOO_ENFORCE_NE(rv, -1, "fcntl: ", strerror(errno));
  if (!on) {
    // Set O_NONBLOCK
    rv |= O_NONBLOCK;
  } else {
    // Clear O_NONBLOCK
    rv &= ~O_NONBLOCK;
  }
  rv = fcntl(fd_, F_SETFL, rv);
  GLOO_ENFORCE_NE(rv, -1, "fcntl: ", strerror(errno));
}

void Socket::configureTimeout(int opt, const std::chrono::milliseconds timeout) {
  struct timeval {
    long long tv_sec;
    long long tv_usec;
  } tv = {
      .tv_sec = timeout.count() / 1000,
      .tv_usec = (timeout.count() % 1000) * 1000,
  };
  auto rv = setsockopt(fd_, SOL_SOCKET, opt, &tv, sizeof(tv));
  GLOO_ENFORCE_NE(rv, -1, "setsockopt: ", strerror(errno));
}

void Socket::recvTimeout(std::chrono::milliseconds timeout) {
  configureTimeout(SO_RCVTIMEO, std::move(timeout));
}

void Socket::sendTimeout(std::chrono::milliseconds timeout) {
  configureTimeout(SO_SNDTIMEO, std::move(timeout));
}

void Socket::bind(const sockaddr_storage& ss) {
  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in* sa = (const struct sockaddr_in*)&ss;
    bind((const struct sockaddr*)sa, sizeof(*sa));
    return;
  }
  if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6* sa = (const struct sockaddr_in6*)&ss;
    bind((const struct sockaddr*)sa, sizeof(*sa));
    return;
  }
  GLOO_ENFORCE(false, "Unknown address family: ", ss.ss_family);
}

void Socket::bind(const struct sockaddr* addr, socklen_t addrlen) {
  auto rv = ::bind(fd_, addr, addrlen);
  GLOO_ENFORCE_NE(rv, -1, "bind: ", strerror(errno));
}

void Socket::listen(int backlog) {
  auto rv = ::listen(fd_, backlog);
  GLOO_ENFORCE_NE(rv, -1, "listen: ", strerror(errno));
}

std::shared_ptr<Socket> Socket::accept() {
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  int rv = -1;
  for (;;) {
    rv = ::accept(fd_, (struct sockaddr*)&addr, &addrlen);
    if (rv == -1) {
      if (errno == EINTR) {
        continue;
      }
      // Return empty shared_ptr to indicate failure.
      // The caller can assume errno has been set.
      return std::shared_ptr<Socket>();
    }
    break;
  }
  return std::make_shared<Socket>(rv);
}

void Socket::connect(const sockaddr_storage& ss) {
  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in* sa = (const struct sockaddr_in*)&ss;
    return connect((const struct sockaddr*)sa, sizeof(*sa));
  }
  if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6* sa = (const struct sockaddr_in6*)&ss;
    return connect((const struct sockaddr*)sa, sizeof(*sa));
  }
  GLOO_ENFORCE(false, "Unknown address family: ", ss.ss_family);
}

void Socket::connect(const struct sockaddr* addr, socklen_t addrlen) {
  for (;;) {
    auto rv = ::connect(fd_, addr, addrlen);
    if (rv == -1) {
      if (errno == EINTR) {
        continue;
      }
      if (errno != EINPROGRESS) {
        GLOO_ENFORCE_NE(rv, -1, "connect: ", strerror(errno));
      }
    }
    break;
  }
}

ssize_t Socket::read(void* buf, size_t count) {
  ssize_t rv = -1;
  for (;;) {
    rv = ::read(fd_, buf, count);
    if (rv == -1 && errno == EINTR) {
      continue;
    }
    break;
  }
  return rv;
}

ssize_t Socket::write(const void* buf, size_t count) {
  ssize_t rv = -1;
  for (;;) {
    rv = ::write(fd_, buf, count);
    if (rv == -1 && errno == EINTR) {
      continue;
    }
    break;
  }
  return rv;
}

Address Socket::sockName() const {
  return Address::fromSockName(fd_);
}

Address Socket::peerName() const {
  return Address::fromPeerName(fd_);
}

Address Socket::safePeerName() const {
  try {
    return Address::fromPeerName(fd_);
  } catch (const EnforceNotMet&) {
    return Address();
  }
}

} // namespace tcp
} // namespace transport
} // namespace gloo
