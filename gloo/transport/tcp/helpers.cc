#include "gloo/transport/tcp/helpers.h"
#include "gloo/common/log.h"
#include "gloo/transport/tcp/debug_data.h"
#include "gloo/transport/tcp/loop.h"

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

void ConnectOperation::handleEvents(Loop& loop, int /*events*/) {
  // Hold a reference to this object to keep it alive until the
  // callback is called.
  auto leak = shared_from_this();
  loop.unregisterDescriptor(socket_->fd(), this);

  int result;
  socklen_t result_len = sizeof(result);
  if (getsockopt(socket_->fd(), SOL_SOCKET, SO_ERROR, &result, &result_len) <
      0) {
    fn_(loop, socket_, SystemError("getsockopt", errno, remote_));
    return;
  }
  if (result != 0) {
    SystemError e("SO_ERROR", result, remote_);
    bool willRetry =
        std::chrono::steady_clock::now() < deadline_ && retry_++ < maxRetries_;

    auto debugData = ConnectDebugData{
        retry_,
        maxRetries_,
        willRetry,
        rank_,
        size_,
        e.what(),
        remote_.str(),
        socket_->sockName().str(),
    };
    GLOO_WARN("{}", debugData);

    // check deadline
    if (willRetry) {
      run(loop);
    } else {
      fn_(loop, socket_, TimeoutError("timed out connecting: " + e.what()));
    }

    return;
  }

  fn_(loop, socket_, Error::kSuccess);
}

} // namespace tcp
} // namespace transport
} // namespace gloo
