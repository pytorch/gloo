// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <string>
#pragma once

namespace gloo {
namespace transport {
namespace tcp {

struct ConnectDebugData {
  const int retryCount;
  const int retryLimit;
  const bool willRetry;
  const int glooRank;
  const int glooSize;
  const std::string error;
  const std::string remote;
  const std::string local;
};

} // namespace tcp
} // namespace transport
} // namespace gloo
