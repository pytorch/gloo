#include "gloo/config.h"

#include <gtest/gtest.h>

#if GLOO_HAVE_TRANSPORT_TCP

#include <gloo/common/utils.h>
#include <gloo/rendezvous/hash_store.h>
#include <gloo/transport/tcp/context.h>
#include <gloo/transport/tcp/device.h>
#include <gloo/transport/tcp/helpers.h>
#include <gloo/transport/tcp/loop.h>

namespace gloo {
namespace transport {
namespace tcp {

TEST(TcpTest, RejectsInvalidPairIdentifierCount) {
  if (useRankAsSeqNumber()) {
    GTEST_SKIP() << "Pair identifiers are unused in rank-as-sequence mode";
  }
  attr attributes;
  attributes.hostname = "127.0.0.1";
  auto device = std::dynamic_pointer_cast<Device>(CreateDevice(attributes));
  if (device->isLazyInit()) {
    GTEST_SKIP() << "Pair identifiers are unused in lazy mode";
  }
  for (size_t count : {size_t(0), size_t(1), size_t(3)}) {
    auto store = std::make_shared<rendezvous::HashStore>();
    Rank remote(
        "peer",
        Address("127.0.0.1", 1).bytes(),
        std::vector<ssize_t>(count, 0));
    store->set("0", remote.bytes());
    auto context = device->createContext(1, 2);
    context->setTimeout(std::chrono::seconds(1));
    try {
      context->createAndConnectAllPairs(store);
      FAIL() << "Expected invalid pair identifier count to be rejected";
    } catch (const EnforceNotMet& error) {
      EXPECT_NE(
          std::string(error.what()).find("Invalid pair identifier count"),
          std::string::npos);
    }
  }
}

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
