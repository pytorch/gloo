/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cmath>

#include "gloo/common/common.h"
#include "gloo/common/enforce.h"
#include "gloo/config.h"
#include "gloo/cuda.h"
#include "gloo/cuda_private.h"

#if GLOO_USE_NCCL
#include "gloo/cuda_collectives_nccl.h"
#else
#include "gloo/cuda_collectives_native.h"
#endif

namespace gloo {

template <typename T, typename Dst>
std::unique_ptr<LocalOp<T>> cudaDeviceReduce(
    std::vector<CudaStream>& streams,
    std::vector<CudaDevicePointer<T>>& devicePtrs,
    Dst& targetPtr,
    const CudaReductionFunction<T>* fn,
    size_t offset,
    size_t count) {
  GLOO_ENFORCE_EQ(streams.size(), devicePtrs.size());

  // Simple copy operation if there is only a single device pointer.
  if (devicePtrs.size() == 1) {
    return make_unique<CudaLocalMemcpy<T, CudaDevicePointer<T>, Dst>>(
        streams[0], devicePtrs[0], targetPtr, offset, count);
  }

#if GLOO_USE_NCCL
  return make_unique<CudaLocalNCCLReduce<T, Dst>>(
      streams, devicePtrs, targetPtr, fn, offset, count);
#else
  return make_unique<CudaLocalNativeReduce<T, Dst>>(
      streams, devicePtrs, targetPtr, fn, offset, count);
#endif
}

template <typename T, typename Src>
std::unique_ptr<LocalOp<T>> cudaDeviceBroadcast(
    std::vector<CudaStream>& streams,
    std::vector<CudaDevicePointer<T>>& devicePtrs,
    Src& sourcePtr,
    size_t offset,
    size_t count) {
  GLOO_ENFORCE_EQ(streams.size(), devicePtrs.size());

  // Simple copy operation if there is only a single device pointer.
  if (devicePtrs.size() == 1) {
    return make_unique<CudaLocalMemcpy<T, Src, CudaDevicePointer<T>>>(
        streams[0], sourcePtr, devicePtrs[0], offset, count);
  }

#if GLOO_USE_NCCL
  return make_unique<CudaLocalNCCLBroadcast<T, Src>>(
      streams, devicePtrs, sourcePtr, offset, count);
#else
  return make_unique<CudaLocalNativeBroadcast<T, Src>>(
      streams, devicePtrs, sourcePtr, offset, count);
#endif
}

} // namespace gloo
