/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 */

#pragma once

#include "gloo/config.h"
#if GLOO_HAVE_TRANSPORT_TCP_TLS

#include <string>

namespace gloo {
namespace test {

extern std::string ca_pkey_file;
extern std::string ca_cert_file;
extern std::string pkey_file;
extern std::string cert_file;

void create_ca_pkey_cert(
    const std::string& _ca_pkey_file,
    const std::string& _ca_cert_file);

void create_pkey_cert(
    const std::string& _cert_file,
    const std::string& _pkey_file,
    const std::string& _ca_pkey_file,
    const std::string& _ca_cert_file);

} // namespace test
} // namespace gloo
#endif
