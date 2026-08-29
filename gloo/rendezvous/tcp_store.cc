/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tcp_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <iostream>
#include <stdexcept>
#include <signal.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#include "gloo/common/error.h"
#include "gloo/common/logging.h"

#define BUFFER_SIZE 1024
#define ACTION_SIZE 3
#define LENGTH_OF_DATA_SIZE 16
#define RESPONSE_SIZE 2

const std::string POST_ACTION_SET = "set";
const std::string POST_ACTION_GET = "get";
const std::string NOT_FOUND = "NOT_FOUND";

namespace gloo
{
  namespace rendezvous
  {
    TCPStore::~TCPStore()
    {
    }

    TCPStore::TCPStore(const std::string &hostname, int port, int world_size, bool is_master, int timeout)
        : hostname_(hostname),
          host_ip_(host_to_ip(hostname)),
          port_(static_cast<uint16_t>(port)),
          world_size_(world_size),
          is_master_(is_master),
          timeout_(timeout),
          data_({}),
          server_fd_(-1)
    {
      if (is_master)
      {
        // create socket
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        server_fd_.reset(server_fd);
        if (server_fd_.get() == -1)
        {
          auto err = std::string("Socket creation failed: ") + strerror(errno);
          GLOO_THROW(err);
        }

        // config server address
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY; // listening on all interfaces
        address.sin_port = htons(port_);

        // bind socket to address
        if (bind(server_fd_.get(), (struct sockaddr *)&address, sizeof(address)) < 0)
        {
          auto err = std::string("Socket bind failed: ") + strerror(errno);
          GLOO_THROW(err);
        }

        // start listening
        if (listen(server_fd_.get(), 3) < 0)
        {
          auto err = std::string("Socket listen failed: ") + strerror(errno);
          GLOO_THROW(err);
        }

        std::thread(&TCPStore::accept_func, this).detach();
      }
    }

    void TCPStore::accept_func()
    {
      while (true)
      {
        int new_socket;
        struct sockaddr_in client_address;
        socklen_t addr_len = sizeof(client_address);
        std::cout << "server fd: <" << server_fd_.get() << ">" << std::endl;
        new_socket = accept(server_fd_.get(), (struct sockaddr *)&client_address, &addr_len);
        if (new_socket < 0)
        {
          auto err = std::string("Accept client connection failed: ") + strerror(errno);
          GLOO_THROW(err);
        }

        // std::cout << "Connection established with client." << std::endl;

        // read action
        char act_buffer[ACTION_SIZE + 1] = {0};
        int valread = read(new_socket, act_buffer, ACTION_SIZE);
        std::string action = std::string(act_buffer);
        if (action == POST_ACTION_SET)
        {

          // read key size
          char key_size_buffer[LENGTH_OF_DATA_SIZE + 1] = {0};
          int valread = read(new_socket, key_size_buffer, LENGTH_OF_DATA_SIZE);
          int key_size = atoi(key_size_buffer);
          // std::cout << "key size: " << key_size << std::endl;

          // read key
          char key_buffer[key_size + 1] = {0};
          valread = read(new_socket, key_buffer, key_size);
          std::string key = std::string(key_buffer);
          // std::cout << "key: " << key << std::endl;

          // read data size
          char data_size_buffer[LENGTH_OF_DATA_SIZE + 1] = {0};
          valread = read(new_socket, data_size_buffer, LENGTH_OF_DATA_SIZE);
          int data_size = atoi(data_size_buffer);
          // std::cout << "data size: " << data_size << std::endl;

          // read data
          char data_buffer[data_size + 1] = {0};
          valread = read(new_socket, data_buffer, data_size);
          std::vector<char> value_vec(data_buffer, data_buffer + data_size);
          // std::cout << "value read: " << valread << "value: <" << data_buffer << ">" << std::endl;

          // update server data_
          mtx.lock();
          data_[key] = value_vec;
          mtx.unlock();

          const char *response = "OK";
          send(new_socket, response, strlen(response), 0);
          // std::cout << "Response sent to client." << std::endl;
        }
        else if (action == POST_ACTION_GET)
        {
          // read key size
          char key_size_buffer[LENGTH_OF_DATA_SIZE + 1] = {0};
          int valread = read(new_socket, key_size_buffer, LENGTH_OF_DATA_SIZE);
          int key_size = atoi(key_size_buffer);

          // read key
          char key_buffer[key_size + 1] = {0};
          valread = read(new_socket, key_buffer, key_size);
          std::string key = std::string(key_buffer);
          // std::cout << "get key: " << key << std::endl;

          bool found = false;
          std::vector<char> value = {};

          mtx.lock();
          if (data_.find(key) != data_.end())
          {
            found = true;
            value = data_[key];
          }
          mtx.unlock();

          if (found)
          {
            send(new_socket, value.data(), value.size(), 0);
          }
          else
          {
            const char *response = NOT_FOUND.c_str();
            send(new_socket, response, strlen(response), 0);
          }
        }
        else
        {
          const char *response = "OK";
          send(new_socket, response, strlen(response), 0);
        }

        close(new_socket);
      }
    }

    int TCPStore::create_server_fd()
    {
      while (true)
      {
        // create socket
        int new_server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (new_server_fd == -1)
        {
          auto err = std::string("Socket creation failed: ") + strerror(errno);
          GLOO_THROW(err);
        }

        // config server address
        struct sockaddr_in server_address;
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port_);

        // set server address ip
        if (inet_pton(AF_INET, host_ip_.c_str(), &server_address.sin_addr) <= 0)
        {
          close(new_server_fd);
          auto err = std::string("Invalid address: ") + strerror(errno);
          GLOO_THROW(err);
        }

        const auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(timeout_);

        // connect to server
        if (connect(new_server_fd, (struct sockaddr *)&server_address, sizeof(server_address)) == 0)
        {
          struct linger so_linger;
          so_linger.l_onoff = 1;  // enable LINGER
          so_linger.l_linger = 0; // send RST to close the connection immediately
          setsockopt(new_server_fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));

          return new_server_fd;
        }

        close(new_server_fd);

        // check timeout
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);
        if (timeout != kNoTimeout && elapsed > timeout)
        {
          GLOO_THROW_IO_EXCEPTION(GLOO_ERROR_MSG(
              "Connection to master timeout for " + std::to_string(timeout_) + " seconds"));
        }
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }

    void TCPStore::set(const std::string &key, const std::vector<char> &data)
    {
      if (is_master_)
      {
        mtx.lock();
        data_[key] = data;
        mtx.unlock();
      }
      else
      {
        // create socket
        int new_server_fd = create_server_fd();

        // send action
        std::string act_data = POST_ACTION_SET;
        const char *message = act_data.c_str();
        send(new_server_fd, message, strlen(message), 0);

        // send key size
        size_t len = key.length();
        std::string len_str = std::to_string(len);
        len_str = std::string(LENGTH_OF_DATA_SIZE - len_str.length(), '0') + len_str;
        message = len_str.c_str();
        send(new_server_fd, message, strlen(message), 0);
        // std::cout << "key size: " << len_str << std::endl;

        // send key
        message = key.c_str();
        send(new_server_fd, message, strlen(message), 0);
        // std::cout << "key: " << key << std::endl;

        // send data size
        len = data.size();
        len_str = std::to_string(len);
        len_str = std::string(LENGTH_OF_DATA_SIZE - len_str.length(), '0') + len_str;
        message = len_str.c_str();
        send(new_server_fd, message, strlen(message), 0);
        // std::cout << "data size: " << len_str << std::endl;

        // send data
        void *data_ptr = static_cast<void *>(const_cast<char *>(data.data()));
        send(new_server_fd, data_ptr, len, 0);

        // get response
        char buffer[RESPONSE_SIZE] = {0};
        int valread = read(new_server_fd, buffer, RESPONSE_SIZE);
        // std::cout << key << " set request, server response: " << buffer << std::endl;

        close(new_server_fd);
      }
    }

    std::vector<char> TCPStore::get(const std::string &key)
    {
      if (is_master_)
      {
        bool found = false;
        std::vector<char> value = {};

        mtx.lock();
        if (data_.find(key) != data_.end())
        {
          found = true;
          value = data_[key];
        }
        mtx.unlock();

        std::string value_str(value.begin(), value.end());
        value_str = found ? value_str : NOT_FOUND;
        return std::vector<char>(value_str.begin(), value_str.end());
      }
      else
      {
        // create socket
        int new_server_fd = create_server_fd();

        // send action
        std::string act_data = POST_ACTION_GET;
        const char *message = act_data.c_str();
        send(new_server_fd, message, strlen(message), 0);
        // std::cout << "Message sent to server." << std::endl;

        // send key size
        size_t len = key.length();
        std::string len_str = std::to_string(len);
        len_str = std::string(LENGTH_OF_DATA_SIZE - len_str.length(), '0') + len_str;
        message = len_str.c_str();
        send(new_server_fd, message, strlen(message), 0);

        // send key
        message = key.c_str();
        send(new_server_fd, message, strlen(message), 0);

        // get response
        char buffer[BUFFER_SIZE] = {0};
        int valread = read(new_server_fd, buffer, BUFFER_SIZE);
        close(new_server_fd);
        if (valread > 0)
        {
          std::string buffer_str = std::string(buffer);
          return std::vector<char>(buffer, buffer + valread);
        }
        else
        {
          GLOO_THROW("Server response failed!");
        }
      }
    }

    void TCPStore::wait(
        const std::vector<std::string> &keys,
        const std::chrono::milliseconds &timeout)
    {
      const auto start = std::chrono::steady_clock::now();
      auto check = [&](const std::vector<std::string> &keys) -> bool
      {
        for (const auto &key : keys)
        {
          auto data = get(key);
          std::string buffer_str(data.begin(), data.end());
          if (buffer_str == NOT_FOUND)
          {
            return false;
          }
        }
        return true;
      };

      while (!check(keys))
      {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);
        if (timeout != kNoTimeout && elapsed > timeout)
        {
          GLOO_THROW_IO_EXCEPTION(GLOO_ERROR_MSG(
              "Wait timeout for key(s): ", ::gloo::MakeString(keys)));
        }
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }

  } // namespace rendezvous
} // namespace gloo
