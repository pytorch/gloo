#include <gloo/common/logging.h>
#include <gloo/transport/tcp/debug_logger.h>

namespace gloo {
namespace transport {
namespace tcp {

void DebugLogger::log(const ConnectDebugData& data) {
  GLOO_ERROR(
      "failed to connect, willRetry=",
      data.willRetry,
      ", retry=",
      data.retryCount,
      ", retryLimit=",
      data.retryLimit,
      ", rank=",
      data.glooRank,
      ", size=",
      data.glooSize,
      ", local=",
      data.local,
      ", remote=",
      data.remote,
      ", error=",
      data.error);
}

} // namespace tcp
} // namespace transport
} // namespace gloo
