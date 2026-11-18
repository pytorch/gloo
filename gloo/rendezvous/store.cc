/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/rendezvous/store.h>

namespace gloo
{
    namespace rendezvous
    {
        constexpr std::chrono::milliseconds Store::kDefaultTimeout;
    } // namespace rendezvous
} // namespace gloo
