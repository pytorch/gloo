// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <gloo/transport/tcp/debug_data.h>

namespace gloo {
namespace transport {
namespace tcp {

class DebugLogger {
 public:
  static void log(const ConnectDebugData& data);

 private:
};

} // namespace tcp
} // namespace transport
} // namespace gloo
