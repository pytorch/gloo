/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <atomic>
#include <list>

#include "gloo/common/error.h"

namespace gloo {


std::list<std::condition_variable *> _cvs;
std::mutex _cvs_mutex;

std::atomic_bool _is_aborted_flag(false);

bool _is_aborted() {
  return _is_aborted_flag.load();
}

void abort() {
  _is_aborted_flag.store(true);
  std::lock_guard<std::mutex> guard(_cvs_mutex);
  for(auto& cv : _cvs) {
    if(cv != NULL) {
      cv->notify_all();
    }
  }
  GLOO_THROW("GLOO ABORTED");
}

void _register_cv(std::condition_variable *cv) {
  std::lock_guard<std::mutex> guard(_cvs_mutex);
  _cvs.push_back(cv);
}

void _deregister_cv(std::condition_variable *cv) {
  std::lock_guard<std::mutex> guard(_cvs_mutex);
  _cvs.remove(cv);
}
} // namespace gloo
