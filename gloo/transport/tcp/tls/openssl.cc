// Copyright (c) Meta Platforms, Inc. and affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef USE_TCP_OPENSSL_LOAD
#include "dynamic_library.h"
namespace {
DynamicLibrary& get_libssl() {
  static DynamicLibrary libssl("libssl.so", "libssl.so.1.1");
  return libssl;
}
} // namespace

#define CALL_SYM(NAME, ...)                                          \
  static auto fn =                                                   \
      reinterpret_cast<decltype(&NAME)>(get_libssl().sym(__func__)); \
  return fn(__VA_ARGS__)
#elif USE_TCP_OPENSSL_LINK
#define CALL_SYM(NAME, ...) return ::NAME(__VA_ARGS__)
#endif

namespace _glootls {

void ERR_print_errors_cb(int (*cb)(const char*, size_t, void*), void* u) {
  CALL_SYM(ERR_print_errors_cb, cb, u);
}

int OPENSSL_init_ssl(uint64_t opts, const OPENSSL_INIT_SETTINGS* settings) {
  CALL_SYM(OPENSSL_init_ssl, opts, settings);
}

const SSL_METHOD* TLS_method(void) {
  CALL_SYM(TLS_method);
}

SSL_CTX* SSL_CTX_new(const SSL_METHOD* method) {
  CALL_SYM(SSL_CTX_new, method);
}

void SSL_CTX_free(SSL_CTX* ctx) {
  CALL_SYM(SSL_CTX_free, ctx);
}

long SSL_CTX_ctrl(SSL_CTX* ctx, int cmd, long larg, void* parg) {
  CALL_SYM(SSL_CTX_ctrl, ctx, cmd, larg, parg);
}

unsigned long SSL_CTX_clear_options(SSL_CTX* ctx, unsigned long op) {
  CALL_SYM(SSL_CTX_clear_options, ctx, op);
}

void SSL_CTX_set_verify_depth(SSL_CTX* ctx, int depth) {
  CALL_SYM(SSL_CTX_set_verify_depth, ctx, depth);
}

void SSL_CTX_set_security_level(SSL_CTX* ctx, int level) {
  CALL_SYM(SSL_CTX_set_security_level, ctx, level);
}

int SSL_CTX_load_verify_locations(
    SSL_CTX* ctx,
    const char* CAfile,
    const char* CApath) {
  CALL_SYM(SSL_CTX_load_verify_locations, ctx, CAfile, CApath);
}

int SSL_CTX_use_certificate_chain_file(SSL_CTX* ctx, const char* file) {
  CALL_SYM(SSL_CTX_use_certificate_chain_file, ctx, file);
}

int SSL_CTX_use_PrivateKey_file(SSL_CTX* ctx, const char* file, int type) {
  CALL_SYM(SSL_CTX_use_PrivateKey_file, ctx, file, type);
}

int SSL_CTX_check_private_key(const SSL_CTX* ctx) {
  CALL_SYM(SSL_CTX_check_private_key, ctx);
}

void SSL_CTX_set_verify(SSL_CTX* ctx, int mode, SSL_verify_cb callback) {
  CALL_SYM(SSL_CTX_set_verify, ctx, mode, callback);
}

int SSL_do_handshake(SSL* s) {
  CALL_SYM(SSL_do_handshake, s);
}

int SSL_get_error(const SSL* s, int ret_code) {
  CALL_SYM(SSL_get_error, s, ret_code);
}

int SSL_write(SSL* ssl, const void* buf, int num) {
  CALL_SYM(SSL_write, ssl, buf, num);
}

int SSL_read(SSL* ssl, void* buf, int num) {
  CALL_SYM(SSL_read, ssl, buf, num);
}

SSL* SSL_new(SSL_CTX* ctx) {
  CALL_SYM(SSL_new, ctx);
}

int SSL_set_fd(SSL* s, int fd) {
  CALL_SYM(SSL_set_fd, s, fd);
}

void SSL_set_connect_state(SSL* s) {
  CALL_SYM(SSL_set_connect_state, s);
}

void SSL_set_accept_state(SSL* s) {
  CALL_SYM(SSL_set_accept_state, s);
}

int SSL_shutdown(SSL* s) {
  CALL_SYM(SSL_shutdown, s);
}

void SSL_free(SSL* ssl) {
  CALL_SYM(SSL_free, ssl);
}

} // namespace _glootls

#undef CALL_SYM
