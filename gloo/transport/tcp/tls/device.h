/**
 * Copyright (c) 2020-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/transport/tcp/device.h"

namespace gloo {
namespace transport {
namespace tcp {
namespace tls {

std::shared_ptr<transport::Device> CreateDevice(
    const struct attr& src,
    std::string pkey_file,
    std::string cert_file,
    std::string ca_file,
    std::string ca_path);

class Device : public ::gloo::transport::tcp::Device {
 public:
  explicit Device(
      const struct attr& attr,
      std::string pkey_file,
      std::string cert_file,
      std::string ca_file,
      std::string ca_path);
  ~Device() override;

  std::shared_ptr<::gloo::transport::Context> createContext(int rank, int size)
      override;

  const std::string& getPKeyFile() const;

  const std::string& getCertFile() const;

  const std::string& getCAFile() const;

  const std::string& getCAPath() const;

 protected:
  const std::string pkey_file_;
  const std::string cert_file_;
  const std::string ca_file_;
  const std::string ca_path_;
};

} // namespace tls
} // namespace tcp
} // namespace transport
} // namespace gloo
