/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#define GLOO_SHARED_STORE

namespace gloo {

class IStore {
 public:
  virtual ~IStore() = default;

  virtual void set(const std::string& key, const std::vector<char>& data) = 0;

  virtual std::vector<char> get(const std::string& key) = 0;

  virtual std::vector<char> wait_get(
      const std::string& key,
      const std::chrono::milliseconds& timeout) {
    wait({key}, timeout);
    return get(key);
  }

  virtual void wait(
      const std::vector<std::string>& keys,
      const std::chrono::milliseconds& timeout) = 0;

  // Extended 2.0 API support
  virtual bool has_v2_support() = 0;

  virtual std::vector<std::vector<char>> multi_get(
      const std::vector<std::string>& keys) = 0;

  virtual void multi_set(
      const std::vector<std::string>& keys,
      const std::vector<std::vector<char>>& values) = 0;

  virtual void append(
      const std::string& key,
      const std::vector<char>& value) = 0;
  virtual int64_t add(const std::string& key, int64_t value) = 0;
};

} // namespace gloo
