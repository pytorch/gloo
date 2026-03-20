/**
 * Copyright (c) 2020-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/test/base_test.h"
#include "gloo/test/openssl_utils.h"

#if GLOO_HAVE_TRANSPORT_IBVERBS
#include <infiniband/verbs.h>
#endif

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

#if GLOO_HAVE_TRANSPORT_IBVERBS
static bool probeIbverbs() {
  int numDevices = 0;
  struct ibv_device** deviceList = ibv_get_device_list(&numDevices);
  if (!deviceList || numDevices == 0) {
    if (deviceList)
      ibv_free_device_list(deviceList);
    return false;
  }
  struct ibv_context* ctx = ibv_open_device(deviceList[0]);
  ibv_free_device_list(deviceList);
  if (!ctx) {
    return false;
  }
  struct ibv_pd* pd = ibv_alloc_pd(ctx);
  if (!pd) {
    ibv_close_device(ctx);
    return false;
  }
  struct ibv_comp_channel* channel = ibv_create_comp_channel(ctx);
  if (!channel) {
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return false;
  }
  struct ibv_cq* cq = ibv_create_cq(ctx, 64, nullptr, channel, 0);
  if (!cq) {
    ibv_destroy_comp_channel(channel);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return false;
  }
  struct ibv_qp_init_attr qpAttr{};
  qpAttr.send_cq = cq;
  qpAttr.recv_cq = cq;
  qpAttr.cap.max_send_wr = 16;
  qpAttr.cap.max_recv_wr = 16;
  qpAttr.cap.max_send_sge = 1;
  qpAttr.cap.max_recv_sge = 1;
  qpAttr.qp_type = IBV_QPT_RC;
  struct ibv_qp* qp = ibv_create_qp(pd, &qpAttr);
  if (!qp) {
    ibv_destroy_cq(cq);
    ibv_destroy_comp_channel(channel);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return false;
  }
  ibv_destroy_qp(qp);
  ibv_destroy_cq(cq);
  ibv_destroy_comp_channel(channel);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  return true;
}
#endif

bool ibverbsAvailable() {
#if GLOO_HAVE_TRANSPORT_IBVERBS
  static bool available = probeIbverbs();
  return available;
#else
  return false;
#endif
}

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
    if (!ibverbsAvailable()) {
      return nullptr;
    }
    gloo::transport::ibverbs::attr attr;
    attr.port = 1;
    try {
      return ::gloo::transport::ibverbs::CreateDevice(attr);
    } catch (const std::exception& e) {
      GLOO_INFO("IBVERBS not available: ", e.what());
    }
  }
#endif
  return nullptr;
}

} // namespace test
} // namespace gloo
