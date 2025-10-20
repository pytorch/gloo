/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 */

#include "gloo/test/openssl_utils.h"
#if GLOO_HAVE_TRANSPORT_TCP_TLS

#include <gloo/common/logging.h>
#include <cstdlib>

namespace gloo {
namespace test {

static constexpr auto debug = " 1>/dev/null 2>/dev/null";
static constexpr auto _and = " && ";

std::string ca_pkey_file;
std::string ca_cert_file;
std::string pkey_file;
std::string cert_file;

void create_ca_pkey_cert(
    const std::string& _ca_pkey_file,
    const std::string& _ca_cert_file) {
  GLOO_ENFORCE(
      0 ==
          std::system(
              (std::string("bash -c \'") + "openssl genrsa -out " +
               _ca_pkey_file + " 2048" + debug + _and +
               "openssl req -subj \"/C=US/ST=New York/L=New York/O=Gloo Certificate Authority\" -new -x509 -days 1 -key " +
               _ca_pkey_file + " -sha256 -extensions v3_ca -out " +
               _ca_cert_file + debug + "\'")
                  .c_str()),
      "Could not create CA private key and certificate");
}

void create_pkey_cert(
    const std::string& _cert_file,
    const std::string& _pkey_file,
    const std::string& _ca_pkey_file,
    const std::string& _ca_cert_file) {
  std::string csr_file = std::string(std::tmpnam(nullptr));
  GLOO_ENFORCE(
      0 ==
          std::system(
              (std::string("bash -c \'") + "openssl genrsa -out " + _pkey_file +
               " 2048" + debug + _and +
               "openssl req -subj \"/C=US/ST=California/L=San Francisco/O=Gloo Testing Company\" -sha256 -new -key " +
               _pkey_file + " -out " + csr_file + debug + _and +
               "openssl x509 -sha256 -req -in " + csr_file + " -CA " +
               _ca_cert_file + " -CAkey " + _ca_pkey_file +
               " -CAcreateserial -out " + _cert_file + " -days 1" + debug +
               _and + "openssl verify -CAfile " + _ca_cert_file + " " +
               _cert_file + debug + "\'")
                  .c_str()),
      "Could not create private key and certificate");
}

namespace {
struct Initializer {
  Initializer() {
    ca_pkey_file = std::string(std::tmpnam(nullptr));
    ca_cert_file = std::string(std::tmpnam(nullptr));
    create_ca_pkey_cert(ca_pkey_file, ca_cert_file);

    pkey_file = std::string(std::tmpnam(nullptr));
    cert_file = std::string(std::tmpnam(nullptr));
    create_pkey_cert(cert_file, pkey_file, ca_pkey_file, ca_cert_file);
  }
};
Initializer initializer;
} // namespace

} // namespace test
} // namespace gloo
#endif
