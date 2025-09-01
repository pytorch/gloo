#pragma once

#include <gloo/transport/remote_key.h>
#include <cstddef> // for size_t
#include <cstdint> // for int64_t, uint32_t
#include <memory> // for std::unique_ptr

namespace gloo {
namespace transport {
namespace ibverbs {

class RemoteKey : public ::gloo::transport::RemoteKey {
 public:
  RemoteKey(int rank, void* addr, size_t size, uint32_t rkey);
  RemoteKey(const RemoteKey& other) = default;
  RemoteKey(RemoteKey&& other) = default;

  virtual ~RemoteKey() = default;

  virtual std::string serialize() const;

  // Static method to deserialize a string and create a new RemoteKey
  static std::unique_ptr<RemoteKey> deserialize(const std::string& str);

 public:
  const void* addr_;
  const uint32_t rkey_;
};

} // namespace ibverbs
} // namespace transport
} // namespace gloo
