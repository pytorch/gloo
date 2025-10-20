/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <chrono>

namespace gloo {
namespace benchmark {

class Timer {
 public:
  Timer() {
    start();
  }

  void start() {
    start_ = std::chrono::high_resolution_clock::now();
  }

  long ns() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::nanoseconds(now - start_).count();
  }

 protected:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// Forward declaration
class Distribution;

// Stores latency samples
class Samples {
 public:
  Samples() {
    constexpr auto capacity = 100 * 1000;
    samples_.reserve(capacity);
  }

  void add(long ns) {
    samples_.push_back(ns);
  }

  void add(const Timer& t) {
    add(t.ns());
  }

  void merge(const Samples& other) {
    samples_.insert(
        samples_.end(), other.samples_.begin(), other.samples_.end());
  }

  long sum() const {
    long result = 0;
    for (auto& sample : samples_) {
      result += sample;
    }
    return result;
  }

 protected:
  std::vector<long> samples_;

  friend class Distribution;
};

// Stores a sorted list of latency samples
class Distribution {
 public:
  explicit Distribution(const Samples& samples) : samples_(samples.samples_) {
    std::sort(samples_.begin(), samples_.end());
  }

  size_t size() const {
    return samples_.size();
  }

  long min() const {
    return samples_[0];
  }

  long max() const {
    return samples_[size() - 1];
  }

  long percentile(float pct) const {
    return samples_[pct * size()];
  }

  long sum() const {
    long result = 0;
    for (auto& sample : samples_) {
      result += sample;
    }
    return result;
  }

 protected:
  std::vector<long> samples_;
};

} // namespace benchmark
} // namespace gloo
