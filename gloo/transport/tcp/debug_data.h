// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
#pragma once

#include <spdlog/fmt/fmt.h>
#include <string>

namespace gloo::transport::tcp {

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

} // namespace gloo::transport::tcp

template <>
struct fmt::formatter<gloo::transport::tcp::ConnectDebugData>
    : fmt::formatter<std::string> {
  auto format(gloo::transport::tcp::ConnectDebugData& data, format_context& ctx)
      const -> decltype(ctx.out()) {
    return fmt::format_to(
        ctx.out(),
        "willRetry={}, retry={}, retryLimit={}, rank={}, size={}, local={}, remote={}, error={}",
        data.willRetry,
        data.retryCount,
        data.retryLimit,
        data.glooRank,
        data.glooSize,
        data.local,
        data.remote,
        data.error);
  }
};
