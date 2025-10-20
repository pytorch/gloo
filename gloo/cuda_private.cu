/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/cuda_private.h"

#include <array>

#include <cuda.h>

// Disable strict aliasing errors for CUDA 9.
#if CUDA_VERSION >= 9000
#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic push
#endif
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif // __GNUC__
#endif // CUDA_VERSION >= 9000
#include <cuda_fp16.h>
#if CUDA_VERSION >= 9000
#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic pop
#endif
#endif // __GNUC__
#endif // CUDA_VERSION >= 9000

#include "gloo/common/common.h"
#include "gloo/types.h"

namespace gloo {

template <typename T>
__global__ void initializeMemory(
    T* ptr,
    const int val,
    const size_t count,
    const size_t stride) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  for (; i < count; i += blockDim.x) {
    ptr[i] = (i * stride) + val;
  }
}

template <>
__global__ void initializeMemory<float16>(
    float16* ptr,
    const int val,
    const size_t count,
    const size_t stride) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  half* ptrAsHalf = (half*)ptr;
  for (; i < count; i += blockDim.x) {
    ptrAsHalf[i] = __float2half(static_cast<float>((i * stride) + val));
  }
}

template <typename T>
CudaMemory<T>::CudaMemory(size_t elements)
    : elements(elements), bytes(elements * sizeof(T)) {
  CUDA_CHECK(cudaGetDevice(&device_));
  // Sychronize memory allocation with NCCL operations
  std::lock_guard<std::mutex> lock(CudaShared::getMutex());
  CUDA_CHECK(cudaMalloc(&ptr_, bytes));
}

template <typename T>
CudaMemory<T>::CudaMemory(CudaMemory<T>&& other) noexcept
    : elements(other.elements),
      bytes(other.bytes),
      device_(other.device_),
      ptr_(other.ptr_) {
  // Nullify pointer on move source
  other.ptr_ = nullptr;
}

template <typename T>
CudaMemory<T>::~CudaMemory() noexcept(false) {
  CudaDeviceScope scope(device_);
  if (ptr_ != nullptr) {
    // Sychronize memory allocation with NCCL operations
    std::lock_guard<std::mutex> lock(CudaShared::getMutex());
    CUDA_CHECK(cudaFree(ptr_));
  }
}

// Instantiate template
template class CudaMemory<float>;
template class CudaMemory<float16>;

// Lookup PCI bus IDs for device.
// As the number of available devices won't change at
// runtime we can seed this cache on the first call.
const std::string& getCudaPCIBusID(int device) {
  static std::once_flag once;
  static std::vector<std::string> busIDs;

  std::call_once(once, []() {
    std::array<char, 16> buf;
    auto count = getDeviceCount();
    busIDs.resize(count);
    for (auto i = 0; i < count; i++) {
      CUDA_CHECK(cudaDeviceGetPCIBusId(buf.data(), buf.size(), i));
      busIDs[i] = buf.data();
    }
  });

  return busIDs[device];
}

} // namespace gloo
