/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/algorithm.h"
#include "gloo/context.h"

namespace gloo {

template <typename T>
class AllreduceLocal : public Algorithm {
 public:
  AllreduceLocal(
      const std::shared_ptr<Context>& context,
      const std::vector<T*>& ptrs,
      const size_t count,
      const ReductionFunction<T>* fn = ReductionFunction<T>::sum);

  virtual ~AllreduceLocal() = default;

  virtual void run() override;

 protected:
  std::vector<T*> ptrs_;
  const size_t count_;
  const size_t bytes_;
  const ReductionFunction<T>* fn_;
};

} // namespace gloo
