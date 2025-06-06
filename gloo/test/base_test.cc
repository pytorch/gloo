/**
 * Copyright (c) 2020-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/test/base_test.h"
#include "gloo/test/openssl_utils.h"

namespace gloo {
namespace test {

const char* kDefaultDevice = "localhost";

// Transports that instantiated algorithms can be tested against.
const std::vector<Transport> kTransportsForClassAlgorithms = {
    Transport::TCP,
    Transport::TCP_LAZY,
#if GLOO_HAVE_TRANSPORT_TCP_TLS
    Transport::TCP_TLS,
#endif
#if GLOO_HAVE_TRANSPORT_IBVERBS
    Transport::IBVERBS,
#endif
};

const std::vector<Transport> kTransportsForRDMA = {
#if GLOO_HAVE_TRANSPORT_IBVERBS
    Transport::IBVERBS,
#endif
};

// Transports that function algorithms can be tested against.
// This is the new style of calling collectives and must be
// preferred over the instantiated style.
const std::vector<Transport> kTransportsForFunctionAlgorithms = {
    Transport::TCP,
    Transport::TCP_LAZY,
#if GLOO_HAVE_TRANSPORT_TCP_TLS
    Transport::TCP_TLS,
#endif
#if GLOO_HAVE_TRANSPORT_UV
    Transport::UV,
#endif
#if GLOO_HAVE_TRANSPORT_IBVERBS
    Transport::IBVERBS,
#endif
};

std::shared_ptr<::gloo::transport::Device> createDevice(Transport transport) {
#if GLOO_HAVE_TRANSPORT_TCP
  if (transport == Transport::TCP) {
    return ::gloo::transport::tcp::CreateDevice(kDefaultDevice);
  } else if (transport == Transport::TCP_LAZY) {
    return ::gloo::transport::tcp::CreateLazyDevice(kDefaultDevice);
  }
#endif
#if GLOO_HAVE_TRANSPORT_TCP_TLS
  if (transport == Transport::TCP_TLS) {
    return ::gloo::transport::tcp::tls::CreateDevice(
        kDefaultDevice, pkey_file, cert_file, ca_cert_file, "");
  }
#endif
#if GLOO_HAVE_TRANSPORT_UV
  if (transport == Transport::UV) {
#ifdef _WIN32
    gloo::transport::uv::attr attr;
    attr.ai_family = AF_UNSPEC;
    return ::gloo::transport::uv::CreateDevice(attr);
#else
    return ::gloo::transport::uv::CreateDevice(kDefaultDevice);
#endif
  }
#endif
#if GLOO_HAVE_TRANSPORT_IBVERBS
  if (transport == Transport::IBVERBS) {
    gloo::transport::ibverbs::attr attr;
    attr.port = 1;
    try {
      return ::gloo::transport::ibverbs::CreateDevice(attr);
    } catch (const InvalidOperationException& e) {
      GLOO_INFO("IBVERBS not available: ", e.what());
    }
  }
#endif
  return nullptr;
}

} // namespace test
} // namespace gloo
