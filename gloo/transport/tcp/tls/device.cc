/**
 * Copyright (c) 2020-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/tcp/tls/device.h"

#include "gloo/transport/tcp/tls/context.h"

namespace gloo {
namespace transport {
namespace tcp {
namespace tls {

std::shared_ptr<transport::Device> CreateDevice(
    const struct attr& src,
    std::string pkey_file,
    std::string cert_file,
    std::string ca_file,
    std::string ca_path) {
  auto device = std::make_shared<Device>(
      CreateDeviceAttr(src),
      std::move(pkey_file),
      std::move(cert_file),
      std::move(ca_file),
      std::move(ca_path));
  return std::shared_ptr<transport::Device>(device);
}

Device::Device(
    const struct attr& attr,
    std::string pkey_file,
    std::string cert_file,
    std::string ca_file,
    std::string ca_path)
    : ::gloo::transport::tcp::Device(attr, /*lazyInit=*/false),
      pkey_file_(std::move(pkey_file)),
      cert_file_(std::move(cert_file)),
      ca_file_(std::move(ca_file)),
      ca_path_(std::move(ca_path)) {}

Device::~Device() {}

std::shared_ptr<transport::Context> Device::createContext(int rank, int size) {
  return std::shared_ptr<transport::Context>(new tls::Context(
      std::dynamic_pointer_cast<Device>(shared_from_this()), rank, size));
}

const std::string& Device::getPKeyFile() const {
  return pkey_file_;
}

const std::string& Device::getCertFile() const {
  return cert_file_;
}

const std::string& Device::getCAFile() const {
  return ca_file_;
}

const std::string& Device::getCAPath() const {
  return ca_path_;
}

} // namespace tls
} // namespace tcp
} // namespace transport
} // namespace gloo
