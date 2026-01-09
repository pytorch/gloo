/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/rendezvous/store.h"

#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace gloo {
namespace rendezvous {

class HashStore : public Store {
 public:
  ~HashStore() override = default;

  void set(const std::string& key, const std::vector<char>& data) override;

  std::vector<char> get(const std::string& key) override;

  void wait(const std::vector<std::string>& keys) override {
    wait(keys, Store::kDefaultTimeout);
  }

  void wait(
      const std::vector<std::string>& keys,
      const std::chrono::milliseconds& timeout) override;

 protected:
  std::unordered_map<std::string, std::vector<char>> map_;
  std::mutex m_;
  std::condition_variable cv_;
};

} // namespace rendezvous
} // namespace gloo
