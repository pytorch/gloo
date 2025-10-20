/**
 * Copyright (c) 2021-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace _glootls {

void ERR_print_errors_cb(int (*cb)(const char*, size_t, void*), void* u);

int OPENSSL_init_ssl(uint64_t opts, const OPENSSL_INIT_SETTINGS* settings);

const SSL_METHOD* TLS_method(void);

SSL_CTX* SSL_CTX_new(const SSL_METHOD* method);

void SSL_CTX_free(SSL_CTX* ctx);

long SSL_CTX_ctrl(SSL_CTX* ctx, int cmd, long larg, void* parg);

unsigned long SSL_CTX_clear_options(SSL_CTX* ctx, unsigned long op);

void SSL_CTX_set_verify_depth(SSL_CTX* ctx, int depth);

void SSL_CTX_set_security_level(SSL_CTX* ctx, int level);

int SSL_CTX_load_verify_locations(
    SSL_CTX* ctx,
    const char* CAfile,
    const char* CApath);

int SSL_CTX_use_certificate_chain_file(SSL_CTX* ctx, const char* file);

int SSL_CTX_use_PrivateKey_file(SSL_CTX* ctx, const char* file, int type);

int SSL_CTX_check_private_key(const SSL_CTX* ctx);

void SSL_CTX_set_verify(SSL_CTX* ctx, int mode, SSL_verify_cb callback);

int SSL_do_handshake(SSL* s);

int SSL_get_error(const SSL* s, int ret_code);

int SSL_write(SSL* ssl, const void* buf, int num);

int SSL_read(SSL* ssl, void* buf, int num);

SSL* SSL_new(SSL_CTX* ctx);

int SSL_set_fd(SSL* s, int fd);

void SSL_set_connect_state(SSL* s);

void SSL_set_accept_state(SSL* s);

int SSL_shutdown(SSL* s);

void SSL_free(SSL* ssl);

} // namespace _glootls
