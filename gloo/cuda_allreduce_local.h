/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/algorithm.h"
#include "gloo/cuda.h"

namespace gloo {

template <typename T>
class CudaAllreduceLocal : public Algorithm {
 public:
  CudaAllreduceLocal(
      const std::shared_ptr<Context>& context,
      const std::vector<T*>& ptrs,
      const int count,
      const std::vector<cudaStream_t>& streams = std::vector<cudaStream_t>());

  virtual ~CudaAllreduceLocal() = default;

  virtual void run() override;

 protected:
  std::vector<CudaDevicePointer<T>> devicePtrs_;
  std::vector<CudaStream> streams_;
  const int count_;
  const int bytes_;
  const CudaReductionFunction<T>* fn_;
  const bool synchronizeDeviceOutputs_;

  std::unique_ptr<LocalOp<T>> localReduceOp_;
  std::unique_ptr<LocalOp<T>> localBroadcastOp_;
};

} // namespace gloo
