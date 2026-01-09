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

namespace gloo {
namespace rendezvous {

class FileStore : public Store {
 public:
  explicit FileStore(const std::string& path);
  ~FileStore() override = default;

  void set(const std::string& key, const std::vector<char>& data) override;

  std::vector<char> get(const std::string& key) override;

  void wait(const std::vector<std::string>& keys) override {
    wait(keys, Store::kDefaultTimeout);
  }

  void wait(
      const std::vector<std::string>& keys,
      const std::chrono::milliseconds& timeout) override;

  std::vector<std::string> getAllKeyFilePaths();

 protected:
  std::string basePath_;

  std::string realPath(const std::string& path);

  std::string tmpPath(const std::string& name);

  std::string objectPath(const std::string& name);

  bool check(const std::vector<std::string>& keys);

  std::vector<std::string> keyFilePaths_;
};

} // namespace rendezvous
} // namespace gloo
