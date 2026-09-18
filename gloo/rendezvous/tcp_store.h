/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "store.h"

#include <condition_variable>
#include <mutex>
#include <map>
#include <netdb.h>
#include <arpa/inet.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#define SOCKET_INIT_TIMEOUT_SECONDS 30

namespace gloo
{
  namespace rendezvous
  {
    class Socket
    {
    public:
      explicit Socket(int fd) : fd_(fd) {}
      ~Socket()
      {
        if (fd_ != -1)
        {
          close(fd_);
        }
      }
      int get() const { return fd_; }
      void reset(int fd = -1)
      {
        if (fd_ != -1)
        {
          close(fd_);
        }
        fd_ = fd;
      }

    private:
      int fd_;
    };

    class TCPStore : public Store
    {
    public:
      explicit TCPStore(const std::string &hostname, int port, int world_size, bool is_master, int timeout = SOCKET_INIT_TIMEOUT_SECONDS);
      virtual ~TCPStore();

      virtual void set(const std::string &key, const std::vector<char> &data)
          override;

      virtual std::vector<char> get(const std::string &key) override;

      virtual void wait(const std::vector<std::string> &keys) override
      {
        auto timeout = std::chrono::seconds(timeout_);
        wait(keys, timeout);
      }

      virtual void wait(
          const std::vector<std::string> &keys,
          const std::chrono::milliseconds &timeout) override;

      virtual void accept_func();

      virtual int create_server_fd();

      std::string host_to_ip(const std::string &host)
      {
        hostent *hostname = gethostbyname(host.c_str());
        if (hostname)
          return std::string(inet_ntoa(**(in_addr **)hostname->h_addr_list));
        return {};
      }

    protected:
      std::string hostname_;
      std::string host_ip_;
      uint16_t port_;
      int world_size_;
      bool is_master_;
      int timeout_;

      std::mutex mtx;

      Socket server_fd_;
      std::map<std::string, std::vector<char>> data_;
    };

  } // namespace rendezvous
} // namespace pygloo
