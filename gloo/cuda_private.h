/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#ifdef __linux__
#include "gloo/common/linux.h"
#endif
#include "gloo/common/logging.h"
#include "gloo/cuda.h"
#include "gloo/transport/device.h"

#if GLOO_USE_TORCH_DTYPES
#include <c10/util/BFloat16.h>
#include <c10/util/Half.h>
#endif

namespace gloo {

#define CUDA_CHECK(condition)       \
  do {                              \
    cudaError_t error = condition;  \
    GLOO_ENFORCE_EQ(                \
        error,                      \
        cudaSuccess,                \
        "Error at: ",               \
        __FILE__,                   \
        ":",                        \
        __LINE__,                   \
        ": ",                       \
        cudaGetErrorString(error)); \
  } while (0)

inline int getCurrentGPUID() {
  int id = 0;
  CUDA_CHECK(cudaGetDevice(&id));
  return id;
}

inline int getGPUIDForPointer(const void* ptr) {
  cudaPointerAttributes attr;
  CUDA_CHECK(cudaPointerGetAttributes(&attr, ptr));
  return attr.device;
}

inline int getDeviceCount() {
  int count;
  CUDA_CHECK(cudaGetDeviceCount(&count));
  return count;
}

const std::string& getCudaPCIBusID(int device);

template <typename T>
int findCudaDevicePointerClosestToDevice(
    std::vector<CudaDevicePointer<T>>& ptrs,
    std::shared_ptr<transport::Device>& dev) {
  // Compute distance between every pointer
  auto devBusID = dev->getPCIBusID();
  std::vector<int> distance(ptrs.size());
  int minDistance = INT_MAX;
  int minDistanceCount = 0;
  for (auto i = 0; i < ptrs.size(); i++) {
#ifdef __linux__
    auto cudaBusID = getCudaPCIBusID(ptrs[i].getDeviceID());
    distance[i] = pciDistance(devBusID, cudaBusID);
#else
    distance[i] = 0;
#endif
    if (distance[i] <= minDistance) {
      if (distance[i] < minDistance) {
        minDistance = distance[i];
        minDistanceCount = 0;
      }
      minDistanceCount++;
    }
  }
  // Choose random pointer closest to device;
  auto minOffset = rand() % minDistanceCount;
  int minIndex = 0;
  for (auto i = 0; i < ptrs.size(); i++) {
    if (distance[i] == minDistance) {
      if (minOffset == 0) {
        minIndex = i;
      }
      minOffset--;
    }
  }
  return minIndex;
}

class CudaDeviceGuard {
 public:
  CudaDeviceGuard() : previous_(getCurrentGPUID()) {}

  ~CudaDeviceGuard() noexcept(false) {
    CUDA_CHECK(cudaSetDevice(previous_));
  }

 private:
  int previous_;
};

class CudaDeviceScope {
 public:
  explicit CudaDeviceScope(int device) : guard_() {
    CUDA_CHECK(cudaSetDevice(device));
  }

 private:
  CudaDeviceGuard guard_;
};

// Managed chunk of GPU memory.
// Convenience class used for tests and benchmarks.
template <typename T>
class CudaMemory {
 public:
  explicit CudaMemory(size_t elements);
  CudaMemory(CudaMemory&&) noexcept;
  ~CudaMemory() noexcept(false);

  T* operator*() const {
    return ptr_;
  }

  const size_t elements;
  const size_t bytes;

 protected:
  CudaMemory(const CudaMemory&) = delete;
  CudaMemory& operator=(const CudaMemory&) = delete;

  int device_;
  T* ptr_;
};

// Container class for a set of per-device streams
class CudaDeviceStreams {
 public:
  CudaDeviceStreams() {
    const int numDevices = getDeviceCount();
    streams_.reserve(numDevices);
    for (auto i = 0; i < numDevices; i++) {
      streams_.emplace_back(i);
    }
  }
  cudaStream_t operator[](const int i) {
    GLOO_ENFORCE_LT(i, streams_.size());
    return *streams_[i];
  }

 protected:
  CudaDeviceStreams(const CudaDeviceStreams&) = delete;
  CudaDeviceStreams& operator=(const CudaDeviceStreams&) = delete;

  std::vector<CudaStream> streams_;
};

} // namespace gloo
