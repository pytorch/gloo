#include <gloo/transport/tcp/helpers.h>

namespace gloo {
namespace transport {
namespace tcp {

void connectLoop(
    Loop& loop,
    const Address& remote,
    const int rank,
    const int size,
    std::chrono::milliseconds timeout,
    typename ConnectOperation::callback_t fn) {
  auto x = std::make_shared<ConnectOperation>(
      remote, rank, size, timeout, std::move(fn));
  x->run(loop);
}

} // namespace tcp
} // namespace transport
} // namespace gloo
