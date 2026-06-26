/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/pair.h"

#include "gloo/common/string.h"

namespace gloo {
namespace transport {

// Have to provide implementation for pure virtual destructor.
Pair::~Pair() {}

std::string Pair::peerDescription() const {
  return ::gloo::MakeString("rank ", getPeerRank(), " (", peer().str(), ")");
}

} // namespace transport
} // namespace gloo
