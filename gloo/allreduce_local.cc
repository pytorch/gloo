/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/allreduce_local.h"

#include <cstring>

namespace gloo {

template <typename T>
AllreduceLocal<T>::AllreduceLocal(
    const std::shared_ptr<Context>& context,
    const std::vector<T*>& ptrs,
    const int count,
    const ReductionFunction<T>* fn)
    : Algorithm(context),
      ptrs_(ptrs),
      count_(count),
      bytes_(count_ * sizeof(T)),
      fn_(fn) {}

template <typename T>
void AllreduceLocal<T>::run() {
  // Reduce specified pointers into ptrs_[0]
  for (int i = 1; i < ptrs_.size(); i++) {
    fn_->call(ptrs_[0], ptrs_[i], count_);
  }
  // Broadcast ptrs_[0]
  for (int i = 1; i < ptrs_.size(); i++) {
    memcpy(ptrs_[i], ptrs_[0], bytes_);
  }
}

// Instantiate templates
#define INSTANTIATE_TEMPLATE(T) template class AllreduceLocal<T>;

INSTANTIATE_TEMPLATE(int8_t);
INSTANTIATE_TEMPLATE(uint8_t);
INSTANTIATE_TEMPLATE(int32_t);
INSTANTIATE_TEMPLATE(int64_t);
INSTANTIATE_TEMPLATE(uint64_t);
INSTANTIATE_TEMPLATE(float);
INSTANTIATE_TEMPLATE(double);
INSTANTIATE_TEMPLATE(float16);
// Needed for benchmark (main.cc) to build, should not get used
INSTANTIATE_TEMPLATE(char);

} // namespace gloo
