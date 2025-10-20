/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdio.h>

#include <uv.h> // @manual

#define UV_CHECK(rv, prefix)  \
  {                           \
    if ((rv) != 0) {          \
      fprintf(                \
          stderr,             \
          "[%s:%d] %s: %s\n", \
          __FILE__,           \
          __LINE__,           \
          prefix,             \
          uv_strerror(rv));   \
    }                         \
  }                           \
  while (0)                   \
    ;

#define FAIL(...)                                                       \
  {                                                                     \
    const auto __str = ::gloo::MakeString(__VA_ARGS__);                 \
    fprintf(stderr, "[%s:%d] %s\n", __FILE__, __LINE__, __str.c_str()); \
    abort();                                                            \
  }                                                                     \
  while (0)                                                             \
    ;
