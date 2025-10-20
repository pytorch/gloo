#include <gtest/gtest.h>

#if GLOO_HAVE_TRANSPORT_TCP

#include <gloo/transport/tcp/helpers.h>
#include <gloo/transport/tcp/loop.h>

namespace gloo {
namespace transport {
namespace tcp {

TEST(TcpTest, ConnectTimeout) {
  Loop loop;

  std::mutex m;
  std::condition_variable cv;
  bool done = false;

  // Use bad address
  auto remote = Address("::1", 10);
  auto timeout = std::chrono::milliseconds(100);
  auto fn = [&](Loop&, std::shared_ptr<Socket>, const Error& e) {
    std::lock_guard<std::mutex> lock(m);
    done = true;
    cv.notify_all();

    EXPECT_TRUE(e);
    EXPECT_TRUE(dynamic_cast<const TimeoutError*>(&e));
  };
  connectLoop(loop, remote, 0, 5, timeout, std::move(fn));

  std::unique_lock<std::mutex> lock(m);
  cv.wait(lock, [&] { return done; });
}

} // namespace tcp
} // namespace transport
} // namespace gloo

#endif // GLOO_HAVE_TRANSPORT_TCP
