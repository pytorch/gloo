#include "gloo/transport/ibverbs/remote_key.h"

#include <cstdint> // for uintptr_t
#include <sstream>
#include <string>

namespace gloo {
namespace transport {
namespace ibverbs {

RemoteKey::RemoteKey(int rank, void* addr, size_t size, uint32_t rkey)
    : transport::RemoteKey(rank, size), addr_(addr), rkey_(rkey) {}

std::string RemoteKey::serialize() const {
  std::ostringstream oss;

  // Format: rank,addr,length,rkey
  oss << rank << "," << reinterpret_cast<uintptr_t>(addr_) << "," << size << ","
      << rkey_;

  return oss.str();
}

std::unique_ptr<RemoteKey> RemoteKey::deserialize(const std::string& str) {
  std::istringstream iss(str);

  int64_t rank;
  uintptr_t addr;
  size_t length;
  uint32_t rkey;
  char comma;

  // Parse format: rank,addr,length,rkey
  if (!(iss >> rank >> comma >> addr >> comma >> length >> comma >> rkey) ||
      comma != ',') {
    // Parsing failed
    return nullptr;
  }

  // Create a new RemoteKey with the parsed values
  return std::make_unique<RemoteKey>(
      rank, reinterpret_cast<void*>(addr), length, rkey);
}

} // namespace ibverbs
} // namespace transport
} // namespace gloo
