/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "prefix_store.h"

#include <sstream>

namespace gloo {
namespace rendezvous {

PrefixStore::PrefixStore(
    const std::string& prefix,
    std::shared_ptr<Store> store)
    : prefix_(prefix), store_(std::move(store)) {}

std::string PrefixStore::joinKey(const std::string& key) {
  std::stringstream ss;
  ss << prefix_ << "/" << key;
  return ss.str();
}

void PrefixStore::set(const std::string& key, const std::vector<char>& data) {
  store_->set(joinKey(key), data);
}

std::vector<char> PrefixStore::get(const std::string& key) {
  return store_->get(joinKey(key));
}

void PrefixStore::wait(
    const std::vector<std::string>& keys,
    const std::chrono::milliseconds& timeout) {
  std::vector<std::string> joinedKeys;
  joinedKeys.reserve(keys.size());
  for (const auto& key : keys) {
    joinedKeys.push_back(joinKey(key));
  }
  store_->wait(joinedKeys, timeout);
}

bool PrefixStore::has_v2_support() {
  return store_->has_v2_support();
}

std::vector<std::vector<char>> PrefixStore::multi_get(
    const std::vector<std::string>& keys) {
  if (!store_->has_v2_support()) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "underlying store doesn't support multi_get");
  }
  std::vector<std::string> prefixed_keys;
  for (auto& key : keys) {
    prefixed_keys.push_back(joinKey(key));
  }
  return store_->multi_get(prefixed_keys);
}

void PrefixStore::multi_set(
    const std::vector<std::string>& keys,
    const std::vector<std::vector<char>>& values) {
  if (!store_->has_v2_support()) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "underlying store doesn't support multi_set");
  }
  std::vector<std::string> prefixed_keys;
  for (auto& key : keys) {
    prefixed_keys.push_back(joinKey(key));
  }
  return store_->multi_set(prefixed_keys, values);
}

void PrefixStore::append(
    const std::string& key,
    const std::vector<char>& data) {
  if (!store_->has_v2_support()) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "underlying store doesn't support append");
  }
  store_->append(joinKey(key), data);
}

int64_t PrefixStore::add(const std::string& key, int64_t value) {
  if (!store_->has_v2_support()) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "underlying store doesn't support append");
  }
  return store_->add(joinKey(key), value);
}

} // namespace rendezvous
} // namespace gloo
