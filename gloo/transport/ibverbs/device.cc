/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/ibverbs/device.h"

#include <fcntl.h>
#include <poll.h>
#include <string.h>

#include <algorithm>

#include "gloo/common/error.h"
#include "gloo/common/linux.h"
#include "gloo/common/logging.h"
#include "gloo/transport/ibverbs/context.h"
#include "gloo/transport/ibverbs/pair.h"

namespace gloo {
namespace transport {
namespace ibverbs {

namespace {
bool hasNvPeerMem() {
  const auto& modules = kernelModules();
  return modules.count("nv_peer_mem") > 0 ||
      // Newer nvidia drivers use a different module name
      modules.count("nvidia_peermem") > 0;
}
} // namespace

// Scope guard for ibverbs device list.
class IbvDevices {
 public:
  IbvDevices() {
    list_ = ibv_get_device_list(&size_);
    if (list_ == nullptr) {
      size_ = 0;
    }
  }

  ~IbvDevices() {
    if (list_ != nullptr) {
      ibv_free_device_list(list_);
    }
  }

  int size() {
    return size_;
  }

  struct ibv_device*& operator[](int i) {
    return list_[i];
  }

 protected:
  int size_;
  struct ibv_device** list_;
};

std::vector<std::string> getDeviceNames() {
  IbvDevices devices;
  std::vector<std::string> deviceNames;
  for (auto i = 0; i < devices.size(); ++i) {
    deviceNames.push_back(devices[i]->name);
  }
  std::sort(deviceNames.begin(), deviceNames.end());
  return deviceNames;
}

std::shared_ptr<::gloo::transport::Device> CreateDevice(
    const struct attr& constAttr) {
  struct attr attr = constAttr;
  IbvDevices devices;

  // Default to using the first device if not specified
  if (attr.name.empty()) {
    if (devices.size() == 0) {
      GLOO_THROW_INVALID_OPERATION_EXCEPTION("No ibverbs devices present");
    }
    std::vector<std::string> names;
    for (auto i = 0; i < devices.size(); i++) {
      GLOO_DEBUG(
          "found candidate device ",
          devices[i]->name,
          " dev=",
          devices[i]->dev_name);
      names.push_back(devices[i]->name);
    }
    std::sort(names.begin(), names.end());
    attr.name = names[0];
  }

  GLOO_INFO(
      "Using ibverbs device=",
      attr.name,
      " port=",
      attr.port,
      " index=",
      attr.index);

  // Look for specified device name
  ibv_context* context = nullptr;
  for (int i = 0; i < devices.size(); i++) {
    if (attr.name == devices[i]->name || attr.name == devices[i]->dev_name) {
      context = ibv_open_device(devices[i]);
      break;
    }
  }
  if (!context) {
    GLOO_THROW_INVALID_OPERATION_EXCEPTION(
        "Unable to find device named: ", attr.name);
  }
  return std::make_shared<Device>(attr, context);
}

Device::Device(const struct attr& attr, ibv_context* context)
    : attr_(attr),
      pciBusID_(infinibandToBusID(attr.name)),
      hasNvPeerMem_(hasNvPeerMem()),
      context_(context) {
  int rv;

  // Query and store device attributes
  rv = ibv_query_device(context_, &deviceAttr_);
  GLOO_ENFORCE_EQ(rv, 0, "ibv_query_device: ", strerror(errno));

  // Query and store port attributes
  rv = ibv_query_port(context_, attr_.port, &portAttr_);
  GLOO_ENFORCE_EQ(rv, 0, "ibv_query_port: ", strerror(errno));

  // Protection domain
  pd_ = ibv_alloc_pd(context_);
  GLOO_ENFORCE(pd_);

  // Completion channel
  comp_channel_ = ibv_create_comp_channel(context_);
  GLOO_ENFORCE(comp_channel_);

  // Start thread to poll completion queue and dispatch
  // completions for completed work requests.
  done_ = false;
  loop_.reset(new std::thread(&Device::loop, this));
}

Device::~Device() {
  int rv;

  done_ = true;
  loop_->join();

  rv = ibv_destroy_comp_channel(comp_channel_);
  GLOO_ENFORCE_EQ(rv, 0, strerror(errno));

  rv = ibv_dealloc_pd(pd_);
  GLOO_ENFORCE_EQ(rv, 0, strerror(errno));

  rv = ibv_close_device(context_);
  GLOO_ENFORCE_EQ(rv, 0, strerror(errno));
}

std::string Device::str() const {
  std::stringstream ss;
  ss << "ibverbs";
  ss << ", pci=" << pciBusID_;
  ss << ", dev=" << attr_.name;
  ss << ", port=" << attr_.port;
  ss << ", index=" << attr_.index;

  // nv_peer_mem module must be loaded for GPUDirect
  if (hasNvPeerMem_) {
    ss << ", gpudirect=ok";
  }

  return ss.str();
}

const std::string& Device::getPCIBusID() const {
  return pciBusID_;
}

bool Device::hasGPUDirect() const {
  return hasNvPeerMem_;
}

std::shared_ptr<transport::Context> Device::createContext(int rank, int size) {
  return std::shared_ptr<transport::Context>(
      new ibverbs::Context(shared_from_this(), rank, size));
}

void Device::loop() {
  int rv;

  auto flags = fcntl(comp_channel_->fd, F_GETFL);
  GLOO_ENFORCE_NE(flags, -1);

  rv = fcntl(comp_channel_->fd, F_SETFL, flags | O_NONBLOCK);
  GLOO_ENFORCE_NE(rv, -1);

  struct pollfd pfd;
  pfd.fd = comp_channel_->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  while (!done_) {
    do {
      rv = poll(&pfd, 1, 10);
    } while ((rv == 0 && !done_) || (rv == -1 && errno == EINTR));
    GLOO_ENFORCE_NE(rv, -1);

    if (done_ && rv == 0) {
      break;
    }

    struct ibv_cq* cq;
    void* cqContext;
    rv = ibv_get_cq_event(comp_channel_, &cq, &cqContext);
    GLOO_ENFORCE_EQ(rv, 0, "ibv_get_cq_event");

    try {
      // Completion queue context is a Pair*.
      // Delegate handling of this event to the pair itself.
      Pair* pair = static_cast<Pair*>(cqContext);
      pair->handleCompletionEvent();
    } catch (const std::exception& ex) {
      GLOO_ERROR("Exception while handling completion event: ", ex.what());
      throw;
    }
  }
}
} // namespace ibverbs
} // namespace transport
} // namespace gloo
