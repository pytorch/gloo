/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

namespace gloo {

std::string getHostname();

// Set the name of the current thread for debugging purposes.
// The name should be 15 characters or less (will be truncated if longer).
void setThreadName(const std::string& name);

bool useRankAsSeqNumber();

bool isStoreExtendedApiEnabled();

bool disableConnectionRetries();

} // namespace gloo
