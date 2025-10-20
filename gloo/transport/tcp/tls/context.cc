/**
 * Copyright (c) 2020-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/transport/tcp/tls/context.h"

#include "gloo/common/logging.h"
#include "gloo/transport/tcp/tls/device.h"
#include "gloo/transport/tcp/tls/openssl.h"
#include "gloo/transport/tcp/tls/pair.h"

namespace {
const char* c_str_or_null(const std::string& s) {
  return s.empty() ? nullptr : s.c_str();
}
} // namespace

namespace gloo {
namespace transport {
namespace tcp {
namespace tls {

std::string getSSLErrorMessage() {
  std::stringstream ss;
  _glootls::ERR_print_errors_cb(
      [](const char* str, size_t sz, void* ss) -> int {
        reinterpret_cast<std::stringstream*>(ss)->write(str, sz);
        return 1; // if (callback(...) <= 0) break
      },
      &ss);
  return ss.str();
}

Context::Context(std::shared_ptr<Device> device, int rank, int size)
    : ::gloo::transport::tcp::Context(
          std::dynamic_pointer_cast<::gloo::transport::tcp::Device>(device),
          rank,
          size),
      ssl_ctx_(
          create_ssl_ctx(
              c_str_or_null(device->getPKeyFile()),
              c_str_or_null(device->getCertFile()),
              c_str_or_null(device->getCAFile()),
              c_str_or_null(device->getCAPath())),
          [](::SSL_CTX* x) { ::_glootls::SSL_CTX_free(x); }) {}

SSL_CTX* Context::create_ssl_ctx(
    const char* pkey,
    const char* cert,
    const char* ca_file,
    const char* ca_path) {
  GLOO_ENFORCE(
      pkey != nullptr && cert != nullptr,
      "Private key and certificate location must be specified");
  GLOO_ENFORCE(
      ca_file != nullptr || ca_path != nullptr,
      "CAfile or CApath must be specified");
  static std::once_flag ssl_ctx_init_;
  std::call_once(ssl_ctx_init_, [] {
    //    SSL_load_error_strings();
    //    SSL_library_init();
    _glootls::OPENSSL_init_ssl(
        OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
        nullptr);
    _glootls::OPENSSL_init_ssl(0, nullptr);
  });
  SSL_CTX* ssl_ctx = _glootls::SSL_CTX_new(_glootls::TLS_method());
  GLOO_ENFORCE(ssl_ctx != nullptr, getSSLErrorMessage());
  GLOO_ENFORCE(
      _glootls::SSL_CTX_set_min_proto_version(ssl_ctx, TLS_MAX_VERSION) == 1,
      getSSLErrorMessage());

  // As we don't need to handle legacy clients,
  // let's remove support for legacy renegotiation:
  _glootls::SSL_CTX_clear_options(ssl_ctx, SSL_OP_LEGACY_SERVER_CONNECT);

  _glootls::SSL_CTX_set_verify_depth(ssl_ctx, 1);

  // To enforcing a higher security level, set it to 3.
  //
  // Level 2
  // Security level set to 112 bits of security. As a result RSA, DSA and
  // DH keys shorter than 2048 bits and ECC keys shorter than 224 bits are
  // prohibited. In addition to the level 1 exclusions any cipher suite using
  // RC4 is also prohibited. SSL version 3 is also not allowed.
  // Compression is disabled.
  //
  // Level 3
  // Security level set to 128 bits of security. As a result RSA,
  // DSA and DHkeys shorter than 3072 bits and ECC keys shorter than 256 bits
  // are prohibited. In addition to the level 2 exclusions cipher suites
  // not offering forward secrecy are prohibited.
  // TLS versions below 1.1 are not permitted. Session tickets are disabled.
  //
  // TODO: should be 3, but it doesn't work yet :(
  _glootls::SSL_CTX_set_security_level(ssl_ctx, 2);

  GLOO_ENFORCE(
      _glootls::SSL_CTX_load_verify_locations(ssl_ctx, ca_file, ca_path) == 1,
      getSSLErrorMessage());
  GLOO_ENFORCE(
      _glootls::SSL_CTX_use_certificate_chain_file(ssl_ctx, cert) == 1,
      getSSLErrorMessage());
  GLOO_ENFORCE(
      _glootls::SSL_CTX_use_PrivateKey_file(ssl_ctx, pkey, SSL_FILETYPE_PEM) ==
          1,
      getSSLErrorMessage());
  GLOO_ENFORCE(
      _glootls::SSL_CTX_check_private_key(ssl_ctx) == 1, getSSLErrorMessage());
  // SSL_VERIFY_PEER
  //
  // Server mode: the server sends a client certificate request to the client.
  // The certificate returned (if any) is checked. If the verification process
  // fails, the TLS/SSL handshake is immediately terminated with an alert
  // message containing the reason for the verification failure.
  // The behaviour can be controlled by the additional
  // SSL_VERIFY_FAIL_IF_NO_PEER_CERT and SSL_VERIFY_CLIENT_ONCE flags.
  //
  // Client mode: the server certificate is verified. If the verification
  // process fails, the TLS/SSL handshake is immediately terminated with
  // an alert message containing the reason for the verification failure.
  // If no server certificate is sent, because an anonymous cipher is used,
  // SSL_VERIFY_PEER is ignored.
  _glootls::SSL_CTX_set_verify(
      ssl_ctx,
      SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT |
          SSL_VERIFY_CLIENT_ONCE,
      nullptr);
  return ssl_ctx;
}

Context::~Context() {}

std::unique_ptr<transport::Pair>& Context::createPair(int rank) {
  pairs_[rank] = std::unique_ptr<transport::Pair>(new tls::Pair(
      this, dynamic_cast<tls::Device*>(device_.get()), rank, getTimeout()));
  return pairs_[rank];
}

} // namespace tls
} // namespace tcp
} // namespace transport
} // namespace gloo
