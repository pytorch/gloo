/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include <infiniband/verbs.h>

#include "gloo/config.h"
#include "gloo/transport/device.h"

// Check that configuration header was properly generated
#if !GLOO_HAVE_TRANSPORT_IBVERBS
#error "Expected GLOO_HAVE_TRANSPORT_IBVERBS to be defined"
#endif

namespace gloo {
namespace transport {
namespace ibverbs {

struct attr {
  std::string name;
  int port;
  int index;
};

// Helper function that returns the list of IB device names in sorted order
std::vector<std::string> getDeviceNames();

std::shared_ptr<::gloo::transport::Device> CreateDevice(const struct attr&);

// Forward declarations
class Pair;
class Buffer;

class Device : public ::gloo::transport::Device,
               public std::enable_shared_from_this<Device> {
  static const int capacity_ = 64;

 public:
  Device(const struct attr& attr, ibv_context* context);
  virtual ~Device();

  virtual std::string str() const override;

  virtual const std::string& getPCIBusID() const override;

  virtual bool hasGPUDirect() const override;

  virtual std::shared_ptr<::gloo::transport::Context> createContext(
      int rank,
      int size) override;

 protected:
  struct attr attr_;
  const std::string pciBusID_;
  const bool hasNvPeerMem_;
  ibv_context* context_;
  ibv_device_attr deviceAttr_;
  ibv_port_attr portAttr_;
  ibv_pd* pd_;
  ibv_comp_channel* comp_channel_;

  void loop();

  std::atomic<bool> done_;
  std::unique_ptr<std::thread> loop_;

  friend class Pair;
  friend class Buffer;
  friend class UnboundBuffer;
};

} // namespace ibverbs
} // namespace transport
} // namespace gloo
