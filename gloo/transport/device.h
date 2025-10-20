/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>

#include "gloo/transport/context.h"
#include "gloo/transport/pair.h"

namespace gloo {
namespace transport {

// Forward declarations
class Pair;
class Buffer;
class UnboundBuffer;

// The device abstraction can be considered as a factory for all
// communication pairs. A communication pair can be associated with
// send and receive buffers. Send buffers serve as the source for one
// sided writes and receive buffers serve as the target of one sided
// writes. Both ends of the pair can create either type of buffer, as
// long as it is paired with the opposite type on the remote end of
// the pair; every receive buffer must be paired with a corresponding
// send buffer and vice versa. The device abstraction may start a
// background thread to handle I/O multiplexing (not configurable).
class Device {
 public:
  virtual ~Device() = 0;

  virtual std::string str() const = 0;

  virtual const std::string& getPCIBusID() const = 0;

  virtual int getInterfaceSpeed() const {
    return 0;
  }

  virtual bool hasGPUDirect() const {
    return false;
  }

  // Factory function to create transport context. A single device may
  // service multiple contexts, with no constraints on this process
  // its rank or the context size.
  virtual std::shared_ptr<Context> createContext(int rank, int size) = 0;
};

} // namespace transport
} // namespace gloo
