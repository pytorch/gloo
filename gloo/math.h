/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "gloo/types.h"

namespace gloo {

template <typename T>
void sum(void* c_, const void* a_, const void* b_, size_t n) {
  T* c = static_cast<T*>(c_);
  const T* a = static_cast<const T*>(a_);
  const T* b = static_cast<const T*>(b_);
  for (auto i = 0; i < n; i++) {
    c[i] = a[i] + b[i];
  }
}

template <typename T>
void sum(T* a, const T* b, size_t n) {
  sum<T>(a, a, b, n);
}

template <typename T>
void product(void* c_, const void* a_, const void* b_, size_t n) {
  T* c = static_cast<T*>(c_);
  const T* a = static_cast<const T*>(a_);
  const T* b = static_cast<const T*>(b_);
  for (auto i = 0; i < n; i++) {
    c[i] = a[i] * b[i];
  }
}

template <typename T>
void product(T* a, const T* b, size_t n) {
  product<T>(a, a, b, n);
}

template <typename T>
void max(void* c_, const void* a_, const void* b_, size_t n) {
  T* c = static_cast<T*>(c_);
  const T* a = static_cast<const T*>(a_);
  const T* b = static_cast<const T*>(b_);
  for (auto i = 0; i < n; i++) {
    c[i] = std::max(a[i], b[i]);
  }
}

template <typename T>
void max(T* a, const T* b, size_t n) {
  max<T>(a, a, b, n);
}

template <typename T>
void min(void* c_, const void* a_, const void* b_, size_t n) {
  T* c = static_cast<T*>(c_);
  const T* a = static_cast<const T*>(a_);
  const T* b = static_cast<const T*>(b_);
  for (auto i = 0; i < n; i++) {
    c[i] = std::min(a[i], b[i]);
  }
}

template <typename T>
void min(T* a, const T* b, size_t n) {
  min<T>(a, a, b, n);
}

template <typename T>
T roundUp(T value, T multiple) {
  T remainder = value % multiple;
  if (remainder == 0) {
    return value;
  }
  return value + multiple - remainder;
}

inline uint32_t log2ceil(uint32_t value) {
  uint32_t dim = 0;
#if defined(__GNUC__)
  if (value <= 1)
    return 0;
  dim = 32 - __builtin_clz(value - 1);
#else
  for (uint32_t size = 1; size < value; ++dim, size <<= 1) /* empty */
    ;
#endif // defined(__GNUC__)
  return dim;
}

#if GLOO_USE_AVX

template <>
void sum<float16>(void* c, const void* a, const void* b, size_t n);
extern template void sum<float16>(
    void* c,
    const void* a,
    const void* b,
    size_t n);

template <>
void product<float16>(void* c, const void* a, const void* b, size_t n);
extern template void product<float16>(
    void* c,
    const void* a,
    const void* b,
    size_t n);

template <>
void max<float16>(void* c, const void* a, const void* b, size_t n);
extern template void max<float16>(
    void* c,
    const void* a,
    const void* b,
    size_t n);

template <>
void min<float16>(void* c, const void* a, const void* b, size_t n);
extern template void min<float16>(
    void* c,
    const void* a,
    const void* b,
    size_t n);

#endif

} // namespace gloo
