#pragma once

#include <string>

namespace gloo {
namespace transport {

class RemoteKey {
 public:
  RemoteKey(int _rank, size_t _size) : rank(_rank), size(_size) {}
  virtual ~RemoteKey() = default;

  virtual std::string serialize() const = 0;

 public:
  const int rank;
  const size_t size;
};

} // namespace transport
} // namespace gloo
