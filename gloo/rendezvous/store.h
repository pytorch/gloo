/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "gloo/common/error.h"
#include "gloo/common/logging.h"
#include "gloo/common/store.h"

// can be used by upstream users to know whether this is available or not.
#define GLOO_STORE_HAS_STORE_V2 1

namespace gloo {
namespace rendezvous {

class Store : public IStore {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout =
      std::chrono::seconds(30);

  ~Store() override = default;

  void set(const std::string& key, const std::vector<char>& data) override = 0;

  std::vector<char> get(const std::string& key) override = 0;

  virtual void wait(const std::vector<std::string>& keys) = 0;

  void wait(
      const std::vector<std::string>& keys,
      const std::chrono::milliseconds& /*timeout*/) override {
    // Base implementation ignores the timeout for backward compatibility.
    // Derived Store implementations should override this function.
    wait(keys);
  }

  bool has_v2_support() override {
    // If True, the following operations are guaranteed to be efficiently and
    // correclty implemented.
    return false;
  }

  std::vector<std::vector<char>> multi_get(
      const std::vector<std::string>& /*keys*/) override {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "this store doesn't support multi_get");
  }

  void multi_set(
      const std::vector<std::string>& /*keys*/,
      const std::vector<std::vector<char>>& /*values*/) override {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "this store doesn't support multi_set");
  }

  void append(const std::string& key, const std::vector<char>& /*data*/)
      override {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION("this store doesn't support append");
  }

  int64_t add(const std::string& key, int64_t value) override {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION("this store doesn't support add");
  }
};

} // namespace rendezvous
} // namespace gloo
